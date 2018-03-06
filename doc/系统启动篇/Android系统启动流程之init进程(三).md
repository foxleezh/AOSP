## 前言

init经过前两个阶段后，已经建立了属性系统和SELinux系统，但是init进程还需要执行很多其他的操作，还要启动许多关键的系统服务，
但是如果都是像属性系统和SELinux系统那样一行行代码去做，显得有点杂乱繁琐，而且不容易扩展，所以Android系统引入了init.rc

init.rc是init进程启动的配置脚本，这个脚本是用一种叫Android Init Language(Android初始化语言)的语言写的，
在7.0以前，init进程只解析根目录下的init.rc文件，但是随着版本的迭代，init.rc越来越臃肿，
所以在7.0以后，init.rc一些业务被分拆到/system/etc/init，/vendor/etc/init，/odm/etc/init三个目录下，
在本篇文章中，我将讲解init.rc的一些语法，然后一步步分析init进程是如何去解析init.rc文件的

本文主要讲解以下内容

- Android Init Language语法

本文涉及到的文件
```
platform/system/core/init/README.md
platform/system/core/init/init.cpp

```

## 一、Android Init Language语法
定义在platform/system/core/init/README.md

源码中已经有一个专门的文档用来说明Android Init Language，应当说这个文档写得还是挺不错的,认真读这个文档的话，基本的语法知识就都知道了,我简单翻译下

### 1.1 Android Init Language

> Android Init Language中由5类语法组成，分别是Actions, Commands, Services, Options, and Imports <br><br>
每一行是一个语句，单词之间用空格分开，如果单词中有空格可以用反斜杠转义，也可以用双引号来引用文本避免和空格冲突，如果一行语句太长可以用 \ 换行，用 # 表示注释 <br><br>
Actions和Services可以作为一个独立的Section,所有的Commands和Options从属于紧挨着的Actions或Services，定义在第一个Section前的Commands和Options将被忽略掉 <br><br>
Actions和Services都是唯一的，如果定义了两个一样的Action，第二个Action的Command将追加到第一个Action，
如果定义了两个一样的Service，第二个Service将被忽略掉并打印错误日志

### 1.2 Init .rc Files
> Android Init Language是用后缀为.rc纯文本编写的,而且是由多个分布在不同目录下的.rc文件组成,如下所述 <br><br>
/init.rc 是最主要的一个.rc文件，它由init进程在初始化时加载，主要负责系统初始化,它会导入 /init.${ro.hardware}.rc ，这个是系统级核心厂商提供的主要.rc文件<br><br>
当执行 mount\_all 语句时，init进程将加载所有在 /{system,vendor,odm}/etc/init/ 目录下的文件，挂载好文件系统后，这些目录将会为Actions和Services服务<br><br>
有一个特殊的目录可能被用来替换上面的三个默认目录，这主要是为了支持工厂模式和其他非标准的启动模式,上面三个目录用于正常的启动过程<br><br>
这三个用于扩展的目录是<br>
1. /system/etc/init/ 用于系统本身，比如SurfaceFlinger, MediaService, and logcatd.<br>
2. /vendor/etc/init/ 用于SoC(系统级核心厂商，如高通),为他们提供一些核心功能和服务<br>
3. /odm/etc/init/ 用于设备制造商（odm定制厂商，如华为、小米），为他们的传感器或外围设备提供一些核心功能和服务<br><br>
所有放在这三个目录下的Services二进制文件都必须有一个对应的.rc文件放在该目录下，并且要在.rc文件中定义service结构,
有一个宏LOCAL\_INIT\_RC,可以帮助开发者处理这个问题. 每个.rc文件还应当包含一些与之相关的actions<br><br>
举个例子，在system/core/logcat目录下有logcatd.rc和Android.mk这两个文件. Android.mk文件中用LOCAL\_INIT\_RC这个宏，在编译时将logcatd.rc放在/system/etc/init/目录下,
init进程在调用 mount\_all 时将其加载，在合适的时机运行其定义的service并将action放入队列<br><br>
将init.rc根据不同服务分拆到不同目录，要比之前放在单个init.rc文件好. 这种方案确保init读取的service和action信息能和同目录下的Services二进制文件更加符合,不再像以前单个init.rc那样.
另外，这样还可以解决多个services加入到系统时发生的冲突，因为他们都拆分到了不同的文件中<br><br>
在 mount\_all 语句中有 "early" 和 "late" 两个可选项，当 early 设置的时候，init进程将跳过被 latemount 标记的挂载操作，并触发fs encryption state 事件，
当 late 被设置的时候，init进程只会执行 latemount 标记的挂载操作，但是会跳过导入的 .rc文件的执行. 默认情况下，不设置任何选项，init进程将执行所有挂载操作

### 1.3 Actions
> Actions由一行行命令组成. trigger用来决定什么时候触发这些命令,当一个事件满足trigger的触发条件时，
这个action就会被加入到处理队列中（除非队列中已经存在）<br><br>
队列中的action按顺序取出执行，action中的命令按顺序执行. 这些命令的执行包含一些活动（设备创建/销毁，属性设置，进程重启）<br><br>
Actions的格式如下：
```
    on <trigger> [&& <trigger>]*
       <command>
       <command>
       <command>
```

### 1.4 Services
> Services是init进程启动的程序,它们也可能在退出时自动重启. Services的格式如下：
```C
    service <name> <pathname> [ <argument> ]*
       <option>
       <option>
       ...
```

### 1.5 Options
> Options是对Services的参数. 它们影响Service如何运行及运行时机<br><br>
`console [<console>]`<br>
Service需要控制台. 第二个参数console的意思是可以设置你想要的控制台类型，默认控制台是/dev/console ,
/dev 这个前缀通常是被忽略的，比如你要设置控制台 /dev/tty0 ,那么只需要设置为console tty0<br><br>
`critical`<br>
表示Service是严格模式. 如果这个Service在4分钟内退出超过4次，那么设备将重启进入recovery模式<br><br>
`disabled`<br>
表示Service不能以class的形式启动，只能以name的形式启动<br><br>
`setenv <name> <value>`<br>
在Service启动时设置name-value的环境变量<br><br>
`socket <name> <type> <perm> [ <user> [ <group> [ <seclabel> ] ] ]`<br>
创建一个unix域的socket,名字叫/dev/socket/_name_ , 并将fd返回给Service. _type_ 只能是 "dgram", "stream" or "seqpacket".
User 和 group 默认值是 0. 'seclabel' 是这个socket的SELinux安全上下文,它的默认值是service安全策略或者基于其可执行文件的安全上下文.
它对应的代码在libcutils的android\_get\_control\_socket<br><br>
`file <path> <type>`<br>
打开一个文件，并将fd返回给这个Service. _type_ 只能是 "r", "w" or "rw". 它对应的代码在libcutils的android\_get\_control\_file<br><br>
`user <username>`<br>
在启动Service前将user改为username,默认启动时user为root(或许默认是无).
在Android M版本，程序必须设置这个值，即使它想有root权限. 以前，一个程序要想有root权限，必须先以root身份运行，然后再降级到所需的uid.
现在已经有一套新的机制取而代之，它通过fs\_config允许厂商赋予特殊二进制文件root权限. 这些说明文档在<http://source.android.com/devices/tech/config/filesystem.html>.
当使用这套新的机制时，程序可以通过user参数选择自己所需的uid,而不需要以root权限运行. 在Android O版本，
程序可以通过capabilities参数直接申请所需的能力，参见下面的capabilities说明<br><br>
`group <groupname> [ <groupname>\* ]`<br>
在启动Service前将group改为第一个groupname,第一个groupname是必须有的，
默认值为root（或许默认值是无），第二个groupname可以不设置，用于追加组（通过setgroups）.<br><br>
`capabilities <capability> [ <capability>\* ]`<br>
在启动Service时将capabilities设置为capability. 'capability' 不能是"CAP\_" prefix, like "NET\_ADMIN" or "SETPCAP". 参考
http://man7.org/linux/man-pages/man7/capabilities.7.html ，里面有capability的说明.<br><br>
`seclabel <seclabel>`<br>
在启动Service前将seclabel设置为seclabel. 主要用于在rootfs上启动的service，比如ueventd, adbd.
在系统分区上运行的service有自己的SELinux安全策略，如果不设置，默认使用init的安全策略.<br><br>
`oneshot`<br>
退出后不再重启<br><br>
`class <name> [ <name>\* ]`<br>
为Service指定class名字. 同一个class名字的Service会被一起启动或退出,默认值是"default",第二个name可以不设置，用于service组.<br><br>
`animation class`<br>
animation class 主要包含为开机动画或关机动画服务的service. 它们很早被启动，而且直到关机最后一步才退出.
它们不允许访问/data 目录，它们可以检查/data目录，但是不能打开 /data 目录，而且需要在 /data 不能用时也正常工作.<br><br>
`onrestart`<br>
在Service重启时执行命令.<br><br>
`writepid <file> [ <file>\* ]`<br>
当Service调用fork时将子进程的pid写入到指定文件. 用于cgroup/cpuset的使用，当/dev/cpuset/下面没有文件但ro.cpuset.default的值却不为空时,
将pid的值写入到/dev/cpuset/_cpuset\_name_/tasks文件中<br><br>
`priority <priority>`<br>
设置进程优先级. 在-20～19之间，默认值是0,能过setpriority实现<br><br>
`namespace <pid|mnt>`<br>
当fork这个service时，设置新的pid和挂载空间<br><br>
`oom_score_adjust <value>`<br>
设置子进程的 /proc/self/oom\_score\_adj 的值为 value,在 -1000 ～ 1000之间.<br><br>

### 1.6 Triggers
> Triggers 是个字符串，当一些事件发生满足该条件时，一些actions就会被执行<br><br>
Triggers分为事件Trigger和属性Trigger<br><br>
事件Trigger由trigger 命令或QueueEventTrigger方法触发.它的格式是个简单的字符串，比如'boot' 或 'late-init'.<br><br>
属性Trigger是在属性被设置或发生改变时触发. 格式是'property:<name>=<value>'或'property:<name>=\*',它会在init初始化设置属性的时候触发.<br><br>
属性Trigger定义的Action可能有多种触发方式，但是事件Trigger定义的Action可能只有一种触发方式<br><br>
比如：<br>
`on boot && property:a=b` 定义了action的触发条件是，boot Trigger触发，并且属性a的值等于b<br><br>
`on property:a=b && property:c=d` 这个定义有三种触发方式:<br>
   1. 在初始化时，属性a=b,属性c=d.
   2. 在属性c=d的情况下，属性a被改为b.
   3. A在属性a=b的情况下，属性c被改为d.

```C
int main(int argc, char** argv) {

    ...

    const BuiltinFunctionMap function_map;
    /*
     * 1.C++中::表示静态方法调用，相当于java中static的方法
     */
    Action::set_function_map(&function_map);


    Parser& parser = Parser::GetInstance();//设置init.rc的解析器
	/*
     * 1.C++中std::make_unique相当于new,它会返回一个std::unique_ptr，即智能指针
     * 2.unique_ptr持有对对象的独有权，两个unique_ptr不能指向一个对象，不能进行复制操作只能进行移动操作
     * 3.移动操作的函数是 p1=std::move(p) ,这样指针p指向的对象就移动到p1上了
     * 4.接下来的这三句代码都是new一个Parser（解析器），然后将它们放到一个map里存起来
     * 5.ServiceParser、ActionParser、ImportParser分别对应service action import的解析
     */
    parser.AddSectionParser("service",std::make_unique<ServiceParser>());
    parser.AddSectionParser("on", std::make_unique<ActionParser>());
    parser.AddSectionParser("import", std::make_unique<ImportParser>());
    std::string bootscript = GetProperty("ro.boot.init_rc", "");
    if (bootscript.empty()) {//如果ro.boot.init_rc没有对应的值，则解析/init.rc以及/system/etc/init、/vendor/etc/init、/odm/etc/init这三个目录下的.rc文件
        parser.ParseConfig("/init.rc");
        parser.set_is_system_etc_init_loaded(
                parser.ParseConfig("/system/etc/init"));
        parser.set_is_vendor_etc_init_loaded(
                parser.ParseConfig("/vendor/etc/init"));
        parser.set_is_odm_etc_init_loaded(parser.ParseConfig("/odm/etc/init"));
    } else {//如果ro.boot.init_rc属性有值就解析属性值
        parser.ParseConfig(bootscript);
        parser.set_is_system_etc_init_loaded(true);
        parser.set_is_vendor_etc_init_loaded(true);
        parser.set_is_odm_etc_init_loaded(true);
    }

    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) parser.DumpState();

    ActionManager& am = ActionManager::GetInstance();

    am.QueueEventTrigger("early-init");//QueueEventTrigger用于触发Action,参数early-init指Action的标记

    // Queue an action that waits for coldboot done so we know ueventd has set up all of /dev...
    am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");
    //QueueBuiltinAction用于添加Action，第一个参数是Action要执行的Command,第二个是Trigger

    // ... so that we can start queuing up actions that require stuff from /dev.
    am.QueueBuiltinAction(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");
    am.QueueBuiltinAction(set_mmap_rnd_bits_action, "set_mmap_rnd_bits");
    am.QueueBuiltinAction(set_kptr_restrict_action, "set_kptr_restrict");
    am.QueueBuiltinAction(keychord_init_action, "keychord_init");
    am.QueueBuiltinAction(console_init_action, "console_init");

    // Trigger all the boot actions to get us started.
    am.QueueEventTrigger("init");

    // Repeat mix_hwrng_into_linux_rng in case /dev/hw_random or /dev/random
    // wasn't ready immediately after wait_for_coldboot_done
    am.QueueBuiltinAction(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");

    // Don't mount filesystems or start core system services in charger mode.
    std::string bootmode = GetProperty("ro.bootmode", "");
    if (bootmode == "charger") {
        am.QueueEventTrigger("charger");
    } else {
        am.QueueEventTrigger("late-init");
    }

    // Run all property triggers based on current state of the properties.
    am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");

    while (true) {
        // By default, sleep until something happens.
        int epoll_timeout_ms = -1;

        if (!(waiting_for_prop || ServiceManager::GetInstance().IsWaitingForExec())) {
            am.ExecuteOneCommand();
        }
        if (!(waiting_for_prop || ServiceManager::GetInstance().IsWaitingForExec())) {
            restart_processes();

            // If there's a process that needs restarting, wake up in time for that.
            if (process_needs_restart_at != 0) {
                epoll_timeout_ms = (process_needs_restart_at - time(nullptr)) * 1000;
                if (epoll_timeout_ms < 0) epoll_timeout_ms = 0;
            }

            // If there's more work to do, wake up again immediately.
            if (am.HasMoreCommands()) epoll_timeout_ms = 0;
        }

        epoll_event ev;
        int nr = TEMP_FAILURE_RETRY(epoll_wait(epoll_fd, &ev, 1, epoll_timeout_ms));
        if (nr == -1) {
            PLOG(ERROR) << "epoll_wait failed";
        } else if (nr == 1) {
            ((void (*)()) ev.data.ptr)();
        }
    }

    return 0;
}
```



