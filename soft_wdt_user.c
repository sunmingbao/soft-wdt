#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define    SOFT_WDT_DEV    "/dev/soft_wdt"

int feed_dog_cnt;
int feed_dog_gap;

int main(int argc, char *argv[])
{
    int i;
    int fd=open("/dev/soft_wdt", O_WRONLY);

    if (fd < 0)
    {
        printf("open %s failed\n", SOFT_WDT_DEV);
        exit(1);
    }


    printf("open %s succeed\n", SOFT_WDT_DEV);

    write(fd, "<name>my_dog</name>", strlen("<name>my_dog</name>"));
    write(fd, "<timeout>123</timeout>", strlen("<timeout>123</timeout>"));
    write(fd, "<stop_on_fd_close>1</stop_on_fd_close>", strlen("<stop_on_fd_close>1</stop_on_fd_close>"));
    write(fd, "<no_reboot>1</no_reboot>", strlen("<no_reboot>1</no_reboot>"));
    
    feed_dog_gap = atoi(argv[1]);
    feed_dog_cnt = atoi(argv[2]);
    for (i=0; i<feed_dog_cnt; i++)
    {
        write(fd, "1234", 4);
        usleep(feed_dog_gap*1000000);
    }

    write(fd, "<stop_dog>1</stop_dog>", strlen("<stop_dog>1</stop_dog>"));
    while (1)
    {
        usleep(1000000);
    }
    
    return 0;
}
