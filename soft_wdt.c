/*
 * soft_wdt.c - software watchdog timer(support multiple dogs)
 *
 * Copyright (C) 2014-2015 Sun Mingbao <sunmingbao@126.com>
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
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <asm/signal.h>

#define MODULE_NAME	"soft_wdt"
#define MOD_VERSION	"2.0"
#define PFX		MODULE_NAME": "

/* time out in seconds */
#define TIMEOUT_MIN		(1)
#define TIMEOUT_MAX		(65536)
#define TIMEOUT_DEFAULT		(5)
#define TIMEOUT_VALID(v)	((v)>=TIMEOUT_MIN && (v)<=TIMEOUT_MAX)

/* module params */
static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int timeout = TIMEOUT_DEFAULT;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout,
	"Watchdog timeout in seconds. (0 < timeout < 65536, default="
	__MODULE_STRING(TIMEOUT_DEFAULT) ")");

static bool no_reboot = 0;
module_param(no_reboot, bool, 0);
MODULE_PARM_DESC(no_reboot,
	"don't reboot the system on dog expire. (default=0)");

static bool core_dump_ill_task = 0;
module_param(core_dump_ill_task, bool, 0);
MODULE_PARM_DESC(core_dump_ill_task,
	"core dump the ill task on dog expire. (default=0)");


/* dog num */
#define MAX_DOG_NUM	(65536)
static int dog_cnt;
struct dog_struct {
	spinlock_t lock;
	struct list_head link_all;
	struct task_struct *owner;
	int id;
	int status;
	char expect_close;
	int is_orphan;
	struct watchdog_info ident;
	int timeout;
	struct timer_list ticktock;
};


#define DOG_NAME(dog)		((dog)->ident.identity)
#define DOG_SET_ALIVE(dog)	((dog)->status=0)
#define DOG_SET_NON_ALIVE(dog)	((dog)->status=1)
#define DOG_ALIVE(dog)		(0==(dog)->status)

static struct list_head all_dogs;
static spinlock_t all_dog_lock;

static void* find_dog_by_id(int id)
{
	struct dog_struct *dog;
	struct list_head *pos;
	list_for_each(pos, &all_dogs) {
		dog = list_entry(pos, struct dog_struct, link_all);
		if (dog->id == id)
			return dog;
	}
	return NULL;
}

static u16 generate_dog_id(void)
{
	static u16 next_id;
	int ret;

	while (find_dog_by_id(next_id)) {
		next_id++;
	}
	ret = next_id;
	next_id++;
	return ret;
}

static void dog_timeout(unsigned long data)
{
	struct dog_struct *dog = (void *)data;

	spin_lock(&dog->lock);
	if (!DOG_ALIVE(dog)) {
		spin_unlock(&dog->lock);
		return;
	}

	DOG_SET_NON_ALIVE(dog);
	spin_unlock(&dog->lock);

	printk(KERN_WARNING PFX "%s expired.", DOG_NAME(dog));
	if (core_dump_ill_task && !(dog->is_orphan)) {
		printk(KERN_WARNING PFX "%s core dump ill task.",
			DOG_NAME(dog));
		send_sig(SIGABRT, dog->owner, 0);
	}

	if (!no_reboot) {
		printk(KERN_WARNING PFX "%s restart the system.",
			DOG_NAME(dog));
		emergency_restart();
	}

}

static void init_dog(struct dog_struct *dog)
{
	DOG_SET_ALIVE(dog);
	spin_lock_init(&(dog->lock));
	dog->owner = current;
	dog->is_orphan = 0;
	dog->timeout = timeout;
	dog->expect_close = 0;
	dog->ticktock =
		(struct timer_list) TIMER_INITIALIZER(dog_timeout,
			0, (unsigned long)dog);
	dog->ident.options =
		(WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE);
	dog->ident.firmware_version = 0;
}


/*
 * operations around dog
 */
static void del_dog(struct dog_struct *dog)
{
	spin_lock(&all_dog_lock);
	list_del(&(dog->link_all));
	dog_cnt--;
	spin_unlock(&all_dog_lock);
}

static int feed_dog(struct dog_struct *dog)
{
	int ret = 0;
	spin_lock(&dog->lock);
	if (DOG_ALIVE(dog)) {
		mod_timer(&(dog->ticktock),
			jiffies + (dog->timeout * HZ));
	} else {
		printk(KERN_WARNING PFX "%s non-alive. invalid feed.",
			DOG_NAME(dog));
		ret = -EIO;
	}
	spin_unlock(&dog->lock);
	return ret;
}

static int add_dog(struct dog_struct *dog)
{
	init_dog(dog);
	spin_lock(&all_dog_lock);
	if (dog_cnt >= MAX_DOG_NUM) {
		spin_unlock(&all_dog_lock);
		return -EUSERS;
	}

	dog->id = generate_dog_id();
	snprintf(DOG_NAME(dog), sizeof (DOG_NAME(dog)),
		"soft_wdt%d", dog->id);
	list_add(&(dog->link_all), &all_dogs);
	dog_cnt++;
	spin_unlock(&all_dog_lock);

	/* start the new dog */
	mod_timer(&(dog->ticktock), jiffies + (dog->timeout * HZ));
	return 0;
}

static int set_dog_timeout(struct dog_struct *dog, int t)
{
	if (!TIMEOUT_VALID(t)) {
		printk(KERN_WARNING PFX "set timeout failed."
			" timeout value %d not between [%d, %d].",
			t, TIMEOUT_MIN, TIMEOUT_MAX);
		return -EINVAL;
	}
	spin_lock(&dog->lock);
	dog->timeout = t;
	mod_timer(&(dog->ticktock), jiffies + (dog->timeout * HZ));
	spin_unlock(&dog->lock);
	printk(KERN_NOTICE PFX "%s timeout set to %d seconds.",
	DOG_NAME(dog), dog->timeout);
	return 0;
}

static void stop_dog(struct dog_struct *dog)
{
	spin_lock(&dog->lock);
	if (!DOG_ALIVE(dog)) {
		spin_unlock(&dog->lock);
		return;
	}

	del_timer(&(dog->ticktock));
	DOG_SET_NON_ALIVE(dog);
	spin_unlock(&dog->lock);
	printk(KERN_NOTICE PFX "%s stopped", DOG_NAME(dog));
}

/*
 * stop all dogs on system down
 */
static int soft_wdt_notify_sys(struct notifier_block *this,
					unsigned long code, void *unused)
{
	struct list_head *head = &all_dogs, *pos;
	struct dog_struct *dog;

	if (code == SYS_DOWN || code == SYS_HALT) {
		spin_lock(&all_dog_lock);
		list_for_each(pos, head) {
			dog = list_entry(pos, struct dog_struct, link_all);
			pos = pos->prev;
			stop_dog(dog);
		}
		spin_unlock(&all_dog_lock);
	}

	return NOTIFY_DONE;
}

/*
 *	file operations
 */

static long soft_wdt_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	void __user *argp = (void __user *) arg;
	int __user *p = argp;
	struct dog_struct *dog = file->private_data;
	int timeout_new;

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		return copy_to_user(argp, &(dog->ident),
				     sizeof (dog->ident)) ? -EFAULT : 0;
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		return put_user(dog->status, p);

	case WDIOC_KEEPALIVE:
		feed_dog(dog);
		return 0;

	case WDIOC_SETTIMEOUT:
		if (get_user(timeout_new, p))
			return -EFAULT;
		if (set_dog_timeout(dog, timeout_new))
			return -EINVAL;

	case WDIOC_GETTIMEOUT:
		return put_user(dog->timeout, p);

	default:
		return -ENOTTY;
	}
}

static int soft_wdt_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct dog_struct *dog;
	dog = kmalloc(sizeof(struct dog_struct), GFP_ATOMIC);
	if (NULL == dog)
		return -ENOMEM;

	ret = add_dog(dog);
	if (ret) {
		kfree(dog);
		return ret;
	}

	file->private_data = dog;
	nonseekable_open(inode, file);
	printk(KERN_NOTICE PFX "%s created.", DOG_NAME(dog));
	return 0;
}

static int soft_wdt_release(struct inode *inode, struct file *file)
{
	struct dog_struct *dog = file->private_data;

	dog->is_orphan = 1;

	if (dog->expect_close == 42 || !DOG_ALIVE(dog)) {
		stop_dog(dog);
		printk(KERN_NOTICE PFX "%s released.", DOG_NAME(dog));
		del_dog(dog);
		kfree(dog);

	}

	return 0;
}

static ssize_t soft_wdt_write(struct file *file, const char __user *data,
						size_t len, loff_t *ppos)
{
	struct dog_struct *dog = file->private_data;
	size_t i;
	char c;

	if (!len)
		goto OUT;

	feed_dog(dog);

	if (nowayout || dog->expect_close != 42)
		goto OUT;

	for (i = 0; i != len; i++) {
		if (get_user(c, data + i))
			return -EFAULT;
		if (c == 'V') {
			dog->expect_close = 42;
			printk(KERN_NOTICE PFX "%s expect close.",
						DOG_NAME(dog));
			goto OUT;
		}
	}

OUT:
	return len;
}


/*
 *	Kernel Interfaces
 */

static const struct file_operations soft_wdt_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.write		= soft_wdt_write,
	.unlocked_ioctl	= soft_wdt_ioctl,
	.open		= soft_wdt_open,
	.release	= soft_wdt_release,
};

static struct miscdevice soft_wdt_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= MODULE_NAME,
	.fops	= &soft_wdt_fops,
};

static struct notifier_block soft_wdt_notifier = {
	.notifier_call	= soft_wdt_notify_sys,
};

static void __init init_inner_structure(void)
{
	dog_cnt = 0;
	spin_lock_init(&all_dog_lock);
	INIT_LIST_HEAD(&all_dogs);
}

static char banner[] __initdata =
	KERN_INFO "software watchdog timer, version: " MOD_VERSION
	" (nowayout=%d; timeout=%d; no_reboot=%d; core_dump_ill_task=%d)\n";

static int __init soft_wdt_init(void)
{
	int ret;
	if (!TIMEOUT_VALID(timeout)) {
		printk(KERN_WARNING PFX
			"timeout value %d not between [%d, %d]."
			"using default value %d",
			timeout, TIMEOUT_MIN, TIMEOUT_MAX, TIMEOUT_DEFAULT);
		timeout = TIMEOUT_DEFAULT;
	}
	init_inner_structure();
	ret = register_reboot_notifier(&soft_wdt_notifier);
	if (ret) {
		printk(KERN_ERR PFX
			"cannot register reboot notifier (err=%d)\n", ret);
		goto OUT;
	}

	ret = misc_register(&soft_wdt_miscdev);
	if (ret) {
		printk(KERN_ERR PFX "cannot register soft_wdt_miscdev");
		goto OUT;
	}

	printk(banner, nowayout, timeout, no_reboot, core_dump_ill_task);

OUT:
	return ret;

}

static void __exit deinit_inner_structure(void)
{
	struct list_head *head = &all_dogs, *pos;
	struct dog_struct *dog;
	spin_lock(&all_dog_lock);

	list_for_each(pos, head) {
		dog = list_entry(pos, struct dog_struct, link_all);
		pos = pos->prev;
		stop_dog(dog);
		printk(KERN_NOTICE PFX "%s released.", DOG_NAME(dog));
		list_del(&(dog->link_all));
		kfree(dog);
	}
	spin_unlock(&all_dog_lock);
}

static void __exit soft_wdt_exit(void)
{
	printk(KERN_INFO PFX "soft wdt quit");
	misc_deregister(&soft_wdt_miscdev);
	deinit_inner_structure();
}

module_init(soft_wdt_init);
module_exit(soft_wdt_exit);

MODULE_AUTHOR("Sun Mingbao <sunmingbao@126.com>");
MODULE_DESCRIPTION("software watchdog timer(support multiple dogs)");
MODULE_VERSION(MOD_VERSION);
MODULE_LICENSE("Dual MIT/GPL");
