
		soft_wdt软件看门狗简介
		
======================

    soft_wdt是一个软件实现的Linux看门狗。

    soft_wdt的主要特点：
        1. 他可以提供大量的看门狗供用户使用; 
        2. 每个看门狗的特性可以单独进行设置;
        3. 所有操作以及重启记录，均可通过日志查看。

    soft_wdt代码编译后，生成一个内核模块soft_wdt.ko。

    模块加载后，将创建一个文件/dev/soft_wdt

    用户态程序，通过系统调用open每打开一次/dev/soft_wdt，就得到一个看门狗，
该看门狗通过open返回的fd进行操作。

    用户每调用一次write系统调用，向fd写入任何数据，就完成了一次喂狗操作。

    如果通过向fd写入如下几种特殊数据，则可实现对看门狗的一些设置。

    <name>x</name>  给看门狗取个名字。x为狗的名字，例如 wangcai  :)。
    <timeout>x</timeout> 设置超时时间，单位为秒。x换成具体数值即可。
    <stop_on_fd_close>x</stop_on_fd_close> 设置关闭fd时，看门狗是否关闭。x=1 close; x=0 not close
    <no_reboot>x</no_reboot> 设置看门狗超时后，是否重启系统。x=1 no reboot; x=0 reboot
    <stop_dog>x</stop_dog>   停止看门狗。 x=1 stop; x=0 do nothing

下面是一些实际的操作示例。

    write(fd, "<name>my_dog</name>", strlen("<name>my_dog</name>"));
    write(fd, "<timeout>123</timeout>", strlen("<timeout>123</timeout>"));
    write(fd, "<stop_on_fd_close>1</stop_on_fd_close>", strlen("<stop_on_fd_close>1</stop_on_fd_close>"));
    write(fd, "<no_reboot>1</no_reboot>", strlen("<no_reboot>1</no_reboot>"));

======================

    本软件是一款开源、免费软件。
    具体版权说明见COPYING.txt。

    本软件的编译及使用见Build.txt。

======================

主要的版本历史：

2014年11月，1.0发布

====================== 
作者: 孙明保(来自 ZTE中兴)
邮箱: sunmingbao@126.com
QQ  : 7743896