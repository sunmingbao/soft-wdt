/*
 * Copyright (C) 2014 Sun Mingbao
 * Dual licensed under the MIT and/or GPL licenses.
 *
 *	Implementation mechanism mainly learned from:
 *	SoftDog	0.07:	A Software Watchdog Device
 *
 *	(c) Copyright 1996 Alan Cox <alan@lxorguk.ukuu.org.uk>,
 *	                        All Rights Reserved.
 */
 
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/rtc.h>

MODULE_AUTHOR("Sun Mingbao <sunmingbao@126.com>");
MODULE_DESCRIPTION("software watchdog timer");
MODULE_VERSION("1.0");
MODULE_LICENSE("Dual MIT/GPL");

#define    WRITE_CONSOLE(fmt, args...) \
    do \
    { \
        printk(KERN_ALERT fmt,##args); \
    } while (0)

#define    DBG_PRINT(fmt, args...) \
    do \
    { \
        WRITE_CONSOLE("DBG:%s(%d)-%s:\n"fmt"\n", __FILE__,__LINE__,__FUNCTION__,##args); \
    } while (0)

#define LOG_AND_WRITE_CONSOLE(fmt, args...) \
    do \
    { \
        WRITE_CONSOLE(fmt"\n", ##args); \
        write_log(fmt, ##args); \
    } while (0)

#define DEFAULT_TIMEOUT    (5)    /* Default is 5 seconds */
static int timeout = DEFAULT_TIMEOUT;	/* in seconds */
static int stop_on_close;

static char *log_file = "/var/log/soft_wdt.log";
module_param(timeout, int, S_IRUGO);
module_param(log_file, charp, S_IRUGO);

struct file *filp_log_file;

#define MAX_USER_DATA_LEN    (255)
#define USER_DATA_BUF_LEN    (MAX_USER_DATA_LEN+1)
typedef struct
{
    struct list_head link_all;
    int id;
    char name[USER_DATA_BUF_LEN];
    int timeout;

    int stop_on_close;
    int no_reboot;
    int is_orphan;
    struct timer_list ticktock;
} t_dog;

static struct list_head all_dogs;
int           dog_cnt;
spinlock_t    all_dog_lock;

static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;
}

static inline void file_pos_write(struct file *file, loff_t pos)
{
	file->f_pos = pos;
}

static int get_time_str(char *output)
{
    struct timex  txc;
    struct rtc_time tm;
    do_gettimeofday(&(txc.time));
    txc.time.tv_sec -= sys_tz.tz_minuteswest * 60;
    rtc_time_to_tm(txc.time.tv_sec,&tm);
    return sprintf(output, "%04d-%02d-%02d %02d:%02d:%02d"
        ,tm.tm_year+1900
        ,tm.tm_mon+1
        ,tm.tm_mday
        ,tm.tm_hour
        ,tm.tm_min
        ,tm.tm_sec);
}

static void kernel_write_file(struct file *filp, const char *buf, int len)
{
    loff_t pos;
    mm_segment_t fs;
    pos = file_pos_read(filp);
    fs=get_fs();  
    set_fs(KERNEL_DS);
    vfs_write(filp, buf, len, &pos);
    file_pos_write(filp, pos);
    set_fs(fs); 
}

static void write_log(const char *fmt, ...)
{
    char actual_contents[512];
    int prefix_len;
	va_list args;
	int user_data_len;

    if (!filp_log_file) return;

    get_time_str(actual_contents+2);
    actual_contents[0]='\n';
    actual_contents[1]='[';
    actual_contents[21]=']';
    prefix_len=22;

	va_start(args, fmt);
	user_data_len=vsnprintf(actual_contents+prefix_len
        ,sizeof(actual_contents)-prefix_len
        ,fmt
        ,args);
	va_end(args);

    kernel_write_file(filp_log_file, actual_contents, prefix_len+user_data_len);
}


static void dog_timeout_proc(unsigned long data)
{
    t_dog *pt_dog = (void *)data;

    LOG_AND_WRITE_CONSOLE("dog[id=%d; name=%s] expired."
        , pt_dog->id
        , pt_dog->name);
    
    if (!pt_dog->no_reboot)
        emergency_restart();
}

static int feed_dog(t_dog *pt_dog)
{
	WRITE_CONSOLE("feed dog %d", pt_dog->id);
	mod_timer(&(pt_dog->ticktock), jiffies+(pt_dog->timeout*HZ));
	return 0;
}

static int set_dog_timeout(t_dog *pt_dog, int t)
{
	if ((t < 0x0001) || (t > 0xFFFF))
		return -EINVAL;

	pt_dog->timeout = t;
    LOG_AND_WRITE_CONSOLE("dog[id=%d; name=%s] timeout set to %d seconds."
        , pt_dog->id
        , pt_dog->name
        , pt_dog->timeout);

	return 0;
}

static int stop_dog(t_dog *pt_dog)
{
    LOG_AND_WRITE_CONSOLE("stop dog[id=%d; name=%s]"
        , pt_dog->id
        , pt_dog->name);
	del_timer(&(pt_dog->ticktock));
	return 0;
}

static int atoi(const char *input)
{
    int ret = 0;
    while (*input!=0) 
    {
        if (*input>='0' && *input<='9')
        {
            ret = 10 * ret + (*input - '0');
        }

        input++;
    }

    return ret;
}

static void update_dog(t_dog *pt_dog, char *user_data)
{
    /* 
       accept bellow data:
       <name>x</name>  x is dog's name
       <timeout>x</timeout> x is timeout value in seconds
       <stop_on_fd_close>x</stop_on_fd_close> x=1 close; x=0 not close
       <no_reboot>x</no_reboot>         x=1 no reboot; x=0 reboot
       <stop_dog>x</stop_dog>           x=1 stop; x=0 do nothing
    */

    char *p_begin, *p_end, *p_value;

    p_begin = strchr(user_data, '<');
    if (NULL==p_begin) return;
    p_begin++;
    
    p_value = strchr(p_begin, '>');
    if (NULL==p_value) return;
    *p_value = 0;
    p_value++;

    p_end = strstr(p_value, "</");
    if (NULL==p_end) return;
    if (p_value==p_end) return;

    *p_end = 0;

    if (0==strcmp(p_begin, "name"))
    {
        strcpy(pt_dog->name, p_value);
        LOG_AND_WRITE_CONSOLE("set dog[id=%d] name to %s"
        , pt_dog->id
        , pt_dog->name);

        return;
    }

    if (0==strcmp(p_begin, "timeout"))
    {
        set_dog_timeout(pt_dog, atoi(p_value));
        return;
    }

    if (0==strcmp(p_begin, "stop_on_fd_close"))
    {
        pt_dog->stop_on_close = atoi(p_value);

        LOG_AND_WRITE_CONSOLE("set dog[id=%d; name=%s] %s stop_on_close"
        , pt_dog->id
        , pt_dog->name
        , pt_dog->stop_on_close?"": "not ");

        return;
    }

    if (0==strcmp(p_begin, "no_reboot"))
    {
        pt_dog->no_reboot = atoi(p_value);
        LOG_AND_WRITE_CONSOLE("set dog[id=%d; name=%s] %s on expire"
        , pt_dog->id
        , pt_dog->name
        , pt_dog->no_reboot?"no_reboot": "reboot");

        return;
    }

    if (0==strcmp(p_begin, "stop_dog"))
    {
        if(1==atoi(p_value))
            stop_dog(pt_dog);

        return;
    }

}

static ssize_t soft_wdt_write(struct file *file, const char __user *data,
			      size_t len, loff_t *ppos)
{
    t_dog *pt_dog = file->private_data;
    char user_data[USER_DATA_BUF_LEN];
	 
	if (len)
    {   
        feed_dog(pt_dog);
    }

	if (len<sizeof(user_data))
    {
        if(copy_from_user(user_data ,data ,len))
		    return -EFAULT;

        user_data[len]=0;
        update_dog(pt_dog, user_data);
    }

	return len;
}

static const struct watchdog_info ident = {
	.options =		WDIOF_SETTIMEOUT |
				WDIOF_KEEPALIVEPING |
				WDIOF_MAGICCLOSE,
	.firmware_version =	0,
	.identity =		"Software Watchdog",
};

static long soft_wdt_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
    t_dog *pt_dog = file->private_data;
	int timeout_new;
    
	switch (cmd) 
    {
        
    	case WDIOC_GETSUPPORT:
    		return copy_to_user(argp, &ident, sizeof(ident)) ? -EFAULT : 0;
            
    	case WDIOC_GETSTATUS:
    	case WDIOC_GETBOOTSTATUS:
    		return put_user(0, p);
            
    	case WDIOC_KEEPALIVE:
    		feed_dog(pt_dog);
    		return 0;
            
    	case WDIOC_SETTIMEOUT:
    		if (get_user(timeout_new, p))
    			return -EFAULT;
    		if (set_dog_timeout(pt_dog, timeout_new))
    			return -EINVAL;
            
    		feed_dog(pt_dog);
    		/* Fall */
    	case WDIOC_GETTIMEOUT:
    		return put_user(pt_dog->timeout, p);
    	default:
    		return -ENOTTY;
	}
}

static void add_dog(t_dog *pt_dog)
{
    pt_dog->timeout = timeout;
    pt_dog->ticktock = 
        (struct timer_list)TIMER_INITIALIZER(dog_timeout_proc, 0, (unsigned long)pt_dog);
    pt_dog->stop_on_close = stop_on_close;
    pt_dog->is_orphan = 0;
    pt_dog->no_reboot = 0;
    pt_dog->name[0] = 0;
    
    spin_lock(&all_dog_lock);
    pt_dog->id = dog_cnt;
    list_add(&(pt_dog->link_all), &all_dogs);
    dog_cnt++;
    spin_unlock(&all_dog_lock);
}

static void del_dog(t_dog *pt_dog)
{
    spin_lock(&all_dog_lock);
    list_del(&(pt_dog->link_all));
    dog_cnt--;
    spin_unlock(&all_dog_lock);

}

static int soft_wdt_open(struct inode *inode, struct file *file)
{
	int ret=0;
    t_dog *pt_dog;
    
    pt_dog = kmalloc(sizeof(t_dog), GFP_ATOMIC);
    add_dog(pt_dog);
    feed_dog(pt_dog);

    file->private_data = pt_dog;
	nonseekable_open(inode, file);

    LOG_AND_WRITE_CONSOLE("create new dog[id=%d]."
        , pt_dog->id);

	return ret;
}

static int soft_wdt_release(struct inode *inode, struct file *file)
{
    t_dog *pt_dog = file->private_data;

    LOG_AND_WRITE_CONSOLE("release dog[id=%d; name=%s]"
        , pt_dog->id
        , pt_dog->name);

    pt_dog->is_orphan = 1;
	if (pt_dog->stop_on_close) 
    {
		stop_dog(pt_dog);
        del_dog(pt_dog);
        kfree(pt_dog);

        LOG_AND_WRITE_CONSOLE("dog[id=%d; name=%s] deleted."
        , pt_dog->id
        , pt_dog->name);
	} 

	return 0;
}

static const struct file_operations soft_wdt_fops = {
	.owner		    = THIS_MODULE,
	.llseek		    = no_llseek,
	.write		    = soft_wdt_write,
	.unlocked_ioctl	= soft_wdt_ioctl,
	.open		    = soft_wdt_open,
	.release	    = soft_wdt_release,
};

static struct miscdevice soft_wdt_miscdev = {
	.minor		= WATCHDOG_MINOR,
	.name		= "soft_wdt",
	.fops		= &soft_wdt_fops,
};


static int __init init_inner_structure(void)
{
    dog_cnt = 0;
    spin_lock_init(&all_dog_lock);
    INIT_LIST_HEAD(&all_dogs);
    
    filp_log_file = filp_open(log_file, O_WRONLY | O_APPEND| O_CREAT, 0640);
    if (IS_ERR(filp_log_file))
    {
        WRITE_CONSOLE("open log file %s failed", log_file);
        return PTR_ERR(filp_log_file);
    }

    return 0;
}

static int __init soft_wdt_init(void)
{
    int retval;

    WRITE_CONSOLE("soft wdt start: timeout=%d", timeout);
    retval=init_inner_structure();
    if (retval < 0)
    {
		goto EXIT;
    }
    
	retval = misc_register(&soft_wdt_miscdev);
	if (retval < 0)
    {
        LOG_AND_WRITE_CONSOLE("misc_register failed");
		goto EXIT;
    }

    LOG_AND_WRITE_CONSOLE("soft wdt start succeed");
    
EXIT:
    return retval;
}


static void __exit deinit_inner_structure(void)
{
    struct list_head *head=&all_dogs, *pos;
    t_dog *pt_dog;
    
    spin_lock(&all_dog_lock);
	list_for_each(pos, head) 
    {
		pt_dog = list_entry(pos, t_dog, link_all);
        pos = pos->prev;
        stop_dog(pt_dog);
        list_del(&(pt_dog->link_all));
        kfree(pt_dog);
	}
    spin_unlock(&all_dog_lock);

    filp_close(filp_log_file, NULL);
}


static void __exit soft_wdt_exit(void)
{
    LOG_AND_WRITE_CONSOLE("soft wdt quit");
	misc_deregister(&soft_wdt_miscdev);
    deinit_inner_structure();
}

module_init(soft_wdt_init);
module_exit(soft_wdt_exit);

