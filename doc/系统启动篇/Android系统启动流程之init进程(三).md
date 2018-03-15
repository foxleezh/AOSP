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
platform/system/core/init/init_parser.cpp
platform/system/core/init/action.cpp
platform/system/core/init/action.h
platform/system/core/init/keyword_map.h
platform/system/core/init/builtins.cpp
platform/system/core/init/service.cpp
platform/system/core/init/service.h
platform/system/core/init/import_parser.cpp
platform/system/core/init/util.cpp
```

## 一、Android Init Language语法
定义在platform/system/core/init/README.md

.rc文件主要配置了两个东西，一个是action,一个是service,trigger和command是对action的补充，options是service的补充.
action加上trigger以及一些command,组成一个Section,service加上一些option，也组成一个Section ，.rc文件就是由一个个Section组成.
.rc文件头部有一个import的语法，表示这些.rc也一并包含并解析,接下来我们重点讲下action和service.

action的格式如下：
```
    on <trigger> [&& <trigger>]*
       <command>
       <command>
       <command>
```
以on开头，trigger是判断条件，command是具体执行一些操作，当满足trigger条件时，执行这些command<br>
trigger可以是一个字符串，如

```C
on early //表示当trigger early或QueueEventTrigger("early")调用时触发
```
也可以是属性，如
```C
on property:sys.boot_from_charger_mode=1//表示当sys.boot_from_charger_mode的值通过property_set设置为1时触发
on property:sys.sysctl.tcp_def_init_rwnd=* // *表示任意值

```
条件可以是多个，用&&连接，如
```C
on zygote-start && property:ro.crypto.state=unencrypted
//表示当zygote-start触发并且ro.crypto.state属性值为unencrypted时触发
```
command就是一些具体的操作，如
```C
mkdir /dev/fscklogs 0770 root system //新建目录
class_stop charger //终止服务
trigger late-init  //触发late-init
```
services的格式如下：
```C
    service <name> <pathname> [ <argument> ]*
       <option>
       <option>
       ...
```
以service开头，name是指定这个服务的名称，pathname表示这个服务的执行文件路径，argument表示执行文件带的参数，option表示这个服务的一些配置
我们看一个典型的例子就知道了
```C
service zygote /system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
    class main
    priority -20
    user root
    group root readproc
    socket zygote stream 660 root system
    onrestart write /sys/android_power/request_state wake
    onrestart write /sys/power/state on
    onrestart restart audioserver
    onrestart restart cameraserver
    onrestart restart media
    onrestart restart netd
    onrestart restart wificond
    writepid /dev/cpuset/foreground/tasks
```
这个是配置在 /init.zygote64_32.rc文件中的service, 它就是我们常说的zygote进程的启动配置

zygote是进程名，可执行文件路径在/system/bin/app_process64，执行文件参数（就是可执行程序main函数里面的那个args）是
-Xzygote /system/bin --zygote --start-system-server --socket-name=zygote

后面的option是一些服务配置，比如
class main表示所属class是main，相当于一个归类，其他service也可以归为main，他们会被一起启动或终止，
service有一个name，也有一个class，就像工作中，你有一个名字叫foxleezh，也可以说你属于android部门.

我上面说的这些东西，源码中已经有一个专门的文档用来说明，路径在platform/system/core/init/README.md,应当说这个文档写得还是挺不错的,认真读这个文档的话，基本的语法知识就都知道了,我简单翻译下
> ### Android Init Language
> Android Init Language中由5类语法组成，分别是Actions, Commands, Services, Options, and Imports <br><br>
每一行是一个语句，单词之间用空格分开，如果单词中有空格可以用反斜杠转义，也可以用双引号来引用文本避免和空格冲突，如果一行语句太长可以用 \ 换行，用 # 表示注释 <br><br>
Actions和Services可以作为一个独立的Section,所有的Commands和Options从属于紧挨着的Actions或Services，定义在第一个Section前的Commands和Options将被忽略掉 <br><br>
Actions和Services都是唯一的，如果定义了两个一样的Action，第二个Action的Command将追加到第一个Action，
如果定义了两个一样的Service，第二个Service将被忽略掉并打印错误日志
> ### Init .rc Files
> Android Init Language是用后缀为.rc的纯文本编写的,而且是由多个分布在不同目录下的.rc文件组成,如下所述 <br><br>
/init.rc 是最主要的一个.rc文件，它由init进程在初始化时加载，主要负责系统初始化,它会导入 /init.${ro.hardware}.rc ，这个是系统级核心厂商提供的主要.rc文件<br><br>
当执行 mount\_all 语句时，init进程将加载所有在 /{system,vendor,odm}/etc/init/ 目录下的文件，挂载好文件系统后，这些目录将会为Actions和Services服务<br><br>
有一个特殊的目录可能被用来替换上面的三个默认目录，这主要是为了支持工厂模式和其他非标准的启动模式,上面三个目录用于正常的启动过程<br><br>
这三个用于扩展的目录是<br>
> 1. /system/etc/init/ 用于系统本身，比如SurfaceFlinger, MediaService, and logcatd.<br>
> 2. /vendor/etc/init/ 用于SoC(系统级核心厂商，如高通),为他们提供一些核心功能和服务<br>
> 3. /odm/etc/init/ 用于设备制造商（odm定制厂商，如华为、小米），为他们的传感器或外围设备提供一些核心功能和服务<br><br>
>
> 所有放在这三个目录下的Services二进制文件都必须有一个对应的.rc文件放在该目录下，并且要在.rc文件中定义service结构,
有一个宏LOCAL\_INIT\_RC,可以帮助开发者处理这个问题. 每个.rc文件还应当包含一些与该服务相关的actions<br><br>
举个例子，在system/core/logcat目录下有logcatd.rc和Android.mk这两个文件. Android.mk文件中用LOCAL\_INIT\_RC这个宏，在编译时将logcatd.rc放在/system/etc/init/目录下,
init进程在调用 mount\_all 时将其加载，在合适的时机运行其定义的service并将action放入队列<br><br>
将init.rc根据不同服务分拆到不同目录，要比之前放在单个init.rc文件好. 这种方案确保init读取的service和action信息能和同目录下的Services二进制文件更加符合,不再像以前单个init.rc那样.
另外，这样还可以解决多个services加入到系统时发生的冲突，因为他们都拆分到了不同的文件中<br><br>
在 mount\_all 语句中有 "early" 和 "late" 两个可选项，当 early 设置的时候，init进程将跳过被 latemount 标记的挂载操作，并触发fs encryption state 事件，
当 late 被设置的时候，init进程只会执行 latemount 标记的挂载操作，但是会跳过导入的 .rc文件的执行. 默认情况下，不设置任何选项，init进程将执行所有挂载操作
> ### Actions
> Actions由一行行命令组成. trigger用来决定什么时候触发这些命令,当一个事件满足trigger的触发条件时，
这个action就会被加入到处理队列中（除非队列中已经存在）<br><br>
队列中的action按顺序取出执行，action中的命令按顺序执行. 这些命令主要用来执行一些操作（设备创建/销毁，属性设置，进程重启）<br><br>
Actions的格式如下：
```
    on <trigger> [&& <trigger>]*
       <command>
       <command>
       <command>
```
> ### Services
> Services是init进程启动的程序,它们也可能在退出时自动重启. Services的格式如下：
```C
    service <name> <pathname> [ <argument> ]*
       <option>
       <option>
       ...
```
> ### Options
> Options是Services的参数配置. 它们影响Service如何运行及运行时机<br><br>
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
它对应的本地实现在libcutils的android\_get\_control\_socket<br><br>
`file <path> <type>`<br>
打开一个文件，并将fd返回给这个Service. _type_ 只能是 "r", "w" or "rw". 它对应的本地实现在libcutils的android\_get\_control\_file<br><br>
`user <username>`<br>
在启动Service前将user改为username,默认启动时user为root(或许默认是无).
在Android M版本，如果一个进程想拥有Linux capabilities（相当于Android中的权限吧），也只能通过设置这个值. 以前，一个程序要想有Linux capabilities，必须先以root身份运行，然后再降级到所需的uid.
现在已经有一套新的机制取而代之，它通过fs\_config允许厂商赋予特殊二进制文件Linux capabilities. 这套机制的说明文档在<http://source.android.com/devices/tech/config/filesystem.html>.
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
当fork这个service时，设置pid或mnt标记<br><br>
`oom_score_adjust <value>`<br>
设置子进程的 /proc/self/oom\_score\_adj 的值为 value,在 -1000 ～ 1000之间.<br><br>
> ### Triggers
> Triggers 是个字符串，当一些事件发生满足该条件时，一些actions就会被执行<br><br>
Triggers分为事件Trigger和属性Trigger<br><br>
事件Trigger由trigger 命令或QueueEventTrigger方法触发.它的格式是个简单的字符串，比如'boot' 或 'late-init'.<br><br>
属性Trigger是在属性被设置或发生改变时触发. 格式是'property:<name>=<value>'或'property:<name>=\*',它会在init初始化设置属性的时候触发.<br><br>
属性Trigger定义的Action可能有多种触发方式，但是事件Trigger定义的Action可能只有一种触发方式<br><br>
比如：<br>
`on boot && property:a=b` 定义了action的触发条件是，boot Trigger触发，并且属性a的值等于b<br><br>
`on property:a=b && property:c=d` 这个定义有三种触发方式:<br>
> 1. 在初始化时，属性a=b,属性c=d.<br>
> 2. 在属性c=d的情况下，属性a被改为b.<br>
> 3. A在属性a=b的情况下，属性c被改为d.<br>
> ### Commands
> `bootchart [start|stop]`<br>
启动或终止bootcharting. 这个出现在init.rc文件中，但是只有在/data/bootchart/enabled文件存在的时候才有效，否则不能工作<br><br>
`chmod <octal-mode> <path>`<br>
修改文件读写权限<br><br>
`chown <owner> <group> <path>`<br>
修改文件所有者或所属用户组<br><br>
`class_start <serviceclass>`<br>
启动所有以serviceclass命名的未启动的service(service有一个name，也有个class，
这里的serviceclass就是class,class_start和后面的start是两种启动方式，class_start是class形式启动，start是name形式启动)<br><br>
`class_stop <serviceclass>`<br>
终止所有以serviceclass命名的正在运行的service<br><br>
`class_reset <serviceclass>`<br>
终止所有以serviceclass命名的正在运行的service,但是不禁用它们. 它们可以稍后被`class_start`重启<br><br>
`class_restart <serviceclass>`<br>
重启所有以serviceclass命名的service<br><br>
`copy <src> <dst>`<br>
复制一个文件，与write相似，比较适合二进制或比较大的文件.<br>
对于src,从链接文件、world-writable或group-writable复制是不允许的.<br>
对于dst，如果目标文件不存在，则默认权限是0600,如果存在就覆盖掉<br><br>
`domainname <name>`<br>
设置域名<br><br>
`enable <servicename>`<br>
将一个禁用的service设置为可用.
如果这个service在运行，那么就会重启.
一般用在bootloader时设置属性，然后启动一个service，比如
on property:ro.boot.myfancyhardware=1
   enable my_fancy_service_for_my_fancy_hardware
`exec [ <seclabel> [ <user> [ <group>\* ] ] ] -- <command> [ <argument>\* ]`
新建子进程并运行一个带指定参数的命令. 这个命令指定了seclabel（安全策略），user(所有者)，group(用户组).
直到这个命令运行完才可以运行其他命令，seclabel可以设置为 - 表示用默认值，argument表示属性值.
直到子进程新建完毕，init进程才继续执行.<br><br>
`exec_start <service>`<br>
启动一个service，只有当执行结果返回，init进程才能继续执行. 这个跟exec相似，只是将一堆参数的设置改在在service中定义<br><br>
`export <name> <value>`<br>
设置环境变量name-value. 这个环境变量将被所有已经启动的service继承<br><br>
`hostname <name>`<br>
设置主机名<br><br>
`ifup <interface>`<br>
开启指定的网络接口<br><br>
`insmod [-f] <path> [<options>]`<br>
安装path下的模块，指定参数options.<br>
-f 表示强制安装，即便是当前Linux内核版本与之不匹配<br><br>
`load_all_props`<br>
加载/system, /vendor等目录下的属性，这个用在init.rc中<br><br>
`load_persist_props`<br>
加载/data 下的持久化属性. 这个用在init.rc中<br><br>
`loglevel <level>`<br>
设置日志输出等级，level表示等级<br><br>
`mkdir <path> [mode] [owner] [group]`<br>
创建一个目录，path是路径，mode是读写权限，默认值是755,owner是所有者，默认值root,group是用户组,默认值是root.
如果该目录已存在，则覆盖他们的mode,owner等设置<br><br>
`mount_all <fstab> [ <path> ]\* [--<option>]`<br>
当手动触发 "early" 和 "late"时，调用fs\_mgr\_mount\_all 函数，指定fstab配置文件，并导入指定目录下的.rc文件
详情可以查看init.rc文件中的有关定义<br><br>
`mount <type> <device> <dir> [ <flag>\* ] [<options>]`<br>
在dir目录下挂载一个名叫device的设备<br>
_flag 包括 "ro", "rw", "remount", "noatime", ...<br>
_options_ 包括 "barrier=1", "noauto\_da\_alloc", "discard", ... 用逗号分开，比如 barrier=1,noauto\_da\_alloc<br><br>
`restart <service>`<br>
终止后重启一个service,如果这个service刚被重启就什么都不做，如果没有在运行，就启动<br><br>
`restorecon <path> [ <path>\* ]`<br>
恢复指定目录下文件的安全上下文.第二个path是安全策略文件. 指定目录不需要必须存在，因为它只需要在init中正确标记<br><br>
`restorecon_recursive <path> [ <path>\* ]`<br>
递归地恢复指定目录下的安全上下文，第二个path是安全策略文件位置<br><br>
`rm <path>`<br>
调用 unlink(2)删除指定文件. 最好用exec -- rm ...代替，因为这样可以确保系统分区已经挂载好<br><br>
`rmdir <path>`<br>
调用 rmdir(2) 删除指定目录<br><br>
`setprop <name> <value>`<br>
设置属性name-value<br><br>
`setrlimit <resource> <cur> <max>`<br>
指定一个进程的资源限制<br><br>
`start <service>`<br>
启动一个未运行的service<br><br>
`stop <service>`<br>
终止一个正在运行的service<br><br>
`swapon_all <fstab>`<br>
调用 fs\_mgr\_swapon\_all，指定fstab配置文件.<br><br>
`symlink <target> <path>`<br>
在path下创建一个指向target的链接<br><br>
`sysclktz <mins_west_of_gmt>`<br>
重置系统基准时间(如果是格林尼治标准时间则设置为0)<br><br>
`trigger <event>`<br>
触发事件event，由一个action触发到另一个action队列<br><br>
`umount <path>`<br>
卸载指定path的文件系统<br><br>
`verity_load_state`<br>
内部实现是加载dm-verity的状态<br><br>
`verity_update_state <mount-point>`<br>
内部实现是设置dm-verity的状态，并且设置partition._mount-point_.verified的属性. 用于adb重新挂载，
因为fs\_mgr 不能直接设置它。<br><br>
`wait <path> [ <timeout> ]`<br>
查看指定路径是否存在. 如果发现则返回,可以设置超时时间，默认值是5秒<br><br>
`wait_for_prop <name> <value>`<br>
等待name属性的值被设置为value，如果name的值一旦被设置为value，马上继续<br><br>
`write <path> <content>`<br>
打开path下的文件，并用write(2)写入content内容. 如果文件不存在就会被创建，如果存在就会被覆盖掉<br><br>
> ### Imports
> import关键字不是一个命令，但是如果有.rc文件包含它就会马上解析它里面的section,用法如下：<br><br>
`import <path>`<br>
解析path下的.rc文件 ，括展当前文件的配置。如果path是个目录，这个目录下所有.rc文件都被解析，但是不会递归,
import被用于以下两个地方：<br>
> 1.在初始化时解析init.rc文件<br>
> 2.在mount_all时解析{system,vendor,odm}/etc/init/等目录下的.rc文件<br><br>
> 后面的内容主要是一些跟调试init进程相关的东西，比如init.svc.<name>可以查看service启动的状态，
ro.boottime.init记录一些关键的时间点，Bootcharting是一个图表化的性能监测工具等,由于与语法关系不大，就不作翻译了

明白了.rc文件的语法，我们再来看看init进程是如何解析.rc文件，将这些语法转化为实际执行的代码的

## 二、 解析.rc文件

之前我们在文档中看到.rc文件主要有根目录下的 /init.rc ，以及{system,vendor,odm}/etc/init/这三个目录下的 *.rc ,
然后就是如果有一个特殊目录被设置的话，就替代这些目录，明白这些，下面的代码就好理解了.

```C
int main(int argc, char** argv) {

    ...

    const BuiltinFunctionMap function_map;
    /*
     * 1.C++中::表示静态方法调用，相当于java中static的方法
     */
    Action::set_function_map(&function_map); //将function_map存放到Action中作为成员属性


    Parser& parser = Parser::GetInstance();//单例模式，得到Parser对象
	/*
     * 1.C++中std::make_unique相当于new,它会返回一个std::unique_ptr，即智能指针，可以自动管理内存
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
```


### 2.1 ParseConfig
定义在 platform/system/core/init/init_parser.cpp

首先是判断传入的是目录还是文件，其实他们都是调用ParseConfigFile，ParseConfigDir就是遍历下该目录中的文件，对文件排个序,然后调用ParseConfigFile.
```C
bool Parser::ParseConfig(const std::string& path) {
    if (is_dir(path.c_str())) {
        return ParseConfigDir(path);
    }
    return ParseConfigFile(path);
}
```
而ParseConfigFile就是读取文件中的数据后，将数据传递给ParseData函数,最后遍历section_parsers_调用其EndFile函数，
EndFile后面再分析，因为是多态实现，我们先看看ParseData

```C
bool Parser::ParseConfigFile(const std::string& path) {
    LOG(INFO) << "Parsing file " << path << "...";
    Timer t;
    std::string data;
    if (!read_file(path, &data)) { //将数据读取到data
        return false;
    }

    data.push_back('\n'); // TODO: fix parse_config.
    ParseData(path, data); //解析数据
    for (const auto& sp : section_parsers_) {
        sp.second->EndFile(path);
    }

    LOG(VERBOSE) << "(Parsing " << path << " took " << t << ".)";
    return true;
}
```

### 2.2 ParseData
ParseData 定义在 platform/system/core/init/init_parser.cpp

ParseData通过调用next_token函数遍历每一个字符，以空格或""为分割将一行拆分成若干个单词，调用T_TEXT放到args数组中，
当读到回车符就调用T_NEWLINE，在section_parsers_这个map中找到对应的on service import的解析器，执行ParseSection，如果在
map中找不到对应的key，就执行ParseLineSection，当读到0的时候，表示文件读取结束，调用T_EOF执行EndSection.

```C
void Parser::ParseData(const std::string& filename, const std::string& data) {
    //TODO: Use a parser with const input and remove this copy
    std::vector<char> data_copy(data.begin(), data.end()); //将data的内容复制到data_copy中
    data_copy.push_back('\0'); //追加一个结束符0

    parse_state state; //定义一个结构体
    state.filename = filename.c_str();
    state.line = 0;
    state.ptr = &data_copy[0];
    state.nexttoken = 0;

    SectionParser* section_parser = nullptr;
    std::vector<std::string> args;

    for (;;) {
        switch (next_token(&state)) { // 遍历data_copy中每一个字符
        case T_EOF: //如果是文件结尾，则调用EndSection
            if (section_parser) {
                section_parser->EndSection();
            }
            return;
        case T_NEWLINE://读取了一行数据
            state.line++;
            if (args.empty()) {
                break;
            }
            /*
             * 1.section_parsers_是一个std:map
             * 2.C++中std:map的count函数是查找key，相当于Java中Map的contains
             * 3.section_parsers_中只有三个key，on service import,之前AddSectionParser函数加入
             */
            if (section_parsers_.count(args[0])) { //判断是否包含 on service import
                if (section_parser) {
                    section_parser->EndSection();
                }
                section_parser = section_parsers_[args[0]].get();//取出对应的parser
                std::string ret_err;
                if (!section_parser->ParseSection(args, &ret_err)) {//解析对应的Section
                    parse_error(&state, "%s\n", ret_err.c_str());
                    section_parser = nullptr;
                }
            } else if (section_parser) { //不包含 on service import则是command或option
                std::string ret_err;
                if (!section_parser->ParseLineSection(args, state.filename,
                                                      state.line, &ret_err)) {//解析command或option
                    parse_error(&state, "%s\n", ret_err.c_str());
                }
            }
            args.clear();
            break;
        case T_TEXT: //将读取的一行数据放到args中，args以空格或""作为分割，将一行数据拆分成单词放进数组中
            args.emplace_back(state.text);
            break;
        }
    }
}

```

这里其实涉及到on service import对应的三个解析器ActionParser,ServiceParser,ImportParser,它们是在之前加入到section_parsers_这个map中的
```C
    Parser& parser = Parser::GetInstance();
    parser.AddSectionParser("service",std::make_unique<ServiceParser>());
    parser.AddSectionParser("on", std::make_unique<ActionParser>());
    parser.AddSectionParser("import", std::make_unique<ImportParser>());

    void Parser::AddSectionParser(const std::string& name,
                                  std::unique_ptr<SectionParser> parser) {
        section_parsers_[name] = std::move(parser);
    }
```

它们都是SectionParser的子类,SectionParser有四个纯虚函数，分别是ParseSection、ParseLineSection、EndSection，EndFile.
```C
class SectionParser {
public:
    virtual ~SectionParser() {
    }
    /*
     * 1.C++中纯虚函数的定义格式是 virtual作为修饰符，然后赋值给0，相当于Java中的抽象方法
     * 2.如果不赋值给0,却以virtual作为修饰符，这种是虚函数，虚函数可以有方法体，相当于Java中父类的方法，主要用于子类的重载
     * 3.只要包含纯虚函数的类就是抽象类，不能new，只能通过子类实现，这个跟Java一样
     */
    virtual bool ParseSection(const std::vector<std::string>& args,
                              std::string* err) = 0;
    virtual bool ParseLineSection(const std::vector<std::string>& args,
                                  const std::string& filename, int line,
                                  std::string* err) const = 0;
    virtual void EndSection() = 0;
    virtual void EndFile(const std::string& filename) = 0;
};
```
接下来我将分析这三个Perser的ParseSection、ParseLineSection、EndSection，EndFile具体实现

### 2.3 ActionParser
定义在platform/system/core/init/action.cpp

我们先看ParseSection，它先将args中下标1到结尾的数据复制到triggers数组中，实际是调用InitTriggers,将处理结果赋值给action_
```C
bool ActionParser::ParseSection(const std::vector<std::string>& args,
                                std::string* err) {
    std::vector<std::string> triggers(args.begin() + 1, args.end()); //将args复制到triggers中，除去下标0
    if (triggers.size() < 1) {
        *err = "actions must have a trigger";
        return false;
    }

    auto action = std::make_unique<Action>(false);
    if (!action->InitTriggers(triggers, err)) { //调用InitTriggers
        return false;
    }

    action_ = std::move(action);
    return true;
}
```

InitTriggers通过比较是否以"property:"开头，区分trigger的类型，如果是property trigger，就调用ParsePropertyTrigger，
如果是event trigger,就将args的参数赋值给event_trigger_，类型是string
```C
bool Action::InitTriggers(const std::vector<std::string>& args, std::string* err) {
    const static std::string prop_str("property:");
    for (std::size_t i = 0; i < args.size(); ++i) {

        ...

        if (!args[i].compare(0, prop_str.length(), prop_str)) {
            if (!ParsePropertyTrigger(args[i], err)) {
                return false;
            }
        } else {
           ...
            event_trigger_ = args[i];
        }
    }

    return true;
}
```

ParsePropertyTrigger函数先是将字符以"="分割为name-value，然后将name-value存入property_triggers_这个map中
```C
bool Action::ParsePropertyTrigger(const std::string& trigger, std::string* err) {
    const static std::string prop_str("property:");
    std::string prop_name(trigger.substr(prop_str.length())); //截取property:后的内容
    size_t equal_pos = prop_name.find('=');
    if (equal_pos == std::string::npos) {
        *err = "property trigger found without matching '='";
        return false;
    }

    std::string prop_value(prop_name.substr(equal_pos + 1)); //取出value
    prop_name.erase(equal_pos); //删除下标为equal_pos的字符，也就是删除"="

    if (auto [it, inserted] = property_triggers_.emplace(prop_name, prop_value); !inserted) {
    //将name-value存放到map中，emplace相当于put操作
        *err = "multiple property triggers found for same property";
        return false;
    }
    return true;
}
```

从上面看出，ParseSection函数的作用就是构造一个Action对象，将trigger条件记录到Action这个对象中，如果是event trigger就赋值给event_trigger_，
如果是property trigger就存放到property_triggers_这个map中. 接下来我们分析ParseLineSection

ParseLineSection是直接调用Action对象的AddCommand函数
```C
bool ActionParser::ParseLineSection(const std::vector<std::string>& args,
                                    const std::string& filename, int line,
                                    std::string* err) const {
    return action_ ? action_->AddCommand(args, filename, line, err) : false;
}
```

AddCommand看名字就大概知道是添加命令，它首先是做一些参数空值的检查，然后是调用FindFunction查找命令对应的执行函数，
最后将这些信息包装成Command对象存放到commands_数组中，这里比较关键的就是FindFunction

```C
bool Action::AddCommand(const std::vector<std::string>& args,
                        const std::string& filename, int line, std::string* err) {
    ... //一些参数检查

    auto function = function_map_->FindFunction(args[0], args.size() - 1, err);//查找命令对应的执行函数
    if (!function) {
        return false;
    }

    AddCommand(function, args, filename, line);
    return true;
}

void Action::AddCommand(BuiltinFunction f,
                        const std::vector<std::string>& args,
                        const std::string& filename, int line) {
    commands_.emplace_back(f, args, filename, line);//commands_是个数组，emplace_back就相当于add
}
```

FindFunction定义在platform/system/core/init/keyword_map.h

这个函数主要作用是通过命令查找对应的执行函数，比如.rc文件中定义chmod,那我们得找到chmod具体去执行哪个函数. 它首先是通过map()返回一个std:map，调用其find函数，
find相当于Java中的get，但是返回的是entry,可以通过entry ->first和entry ->second获取key-value.
找到的value是一个结构体，里面有三个值，第一个是参数最小数目，第二个是参数最大数目，第三个就是执行函数，
之后作了参数的数目检查，也就是说命令后的参数要在最小值和最大值之间.

```C
const Function FindFunction(const std::string& keyword,
                                size_t num_args,
                                std::string* err) const {
        using android::base::StringPrintf;

        auto function_info_it = map().find(keyword); //找到keyword对应的entry
        if (function_info_it == map().end()) { // end是最后一个元素后的元素，表示找不到
            *err = StringPrintf("invalid keyword '%s'", keyword.c_str());
            return nullptr;
        }

        auto function_info = function_info_it->second;//获取value

        auto min_args = std::get<0>(function_info);//获取参数数量最小值
        auto max_args = std::get<1>(function_info);//获取参数数量最大值
        if (min_args == max_args && num_args != min_args) {//将实际参数数量与最大值最小值比较
            *err = StringPrintf("%s requires %zu argument%s",
                                keyword.c_str(), min_args,
                                (min_args > 1 || min_args == 0) ? "s" : "");
            return nullptr;
        }

        if (num_args < min_args || num_args > max_args) {
            if (max_args == std::numeric_limits<decltype(max_args)>::max()) {
                *err = StringPrintf("%s requires at least %zu argument%s",
                                    keyword.c_str(), min_args,
                                    min_args > 1 ? "s" : "");
            } else {
                *err = StringPrintf("%s requires between %zu and %zu arguments",
                                    keyword.c_str(), min_args, max_args);
            }
            return nullptr;
        }

        return std::get<Function>(function_info);//返回命令对应的执行函数
    }
```

我们看看map()的实现,定义在platform/system/core/init/builtins.cpp

这个实现比较简单，就是直接构造一个map，然后返回. 比如{"bootchart", {1,1,do_bootchart}},
表示命令名称叫bootchart，对应的执行函数是do_bootchart，允许传入的最小和最大参数数量是1
```C
BuiltinFunctionMap::Map& BuiltinFunctionMap::map() const {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max(); //表示size_t的最大值
    // clang-format off
    static const Map builtin_functions = {
        {"bootchart",               {1,     1,    do_bootchart}},
        {"chmod",                   {2,     2,    do_chmod}},
        {"chown",                   {2,     3,    do_chown}},
        {"class_reset",             {1,     1,    do_class_reset}},
        {"class_restart",           {1,     1,    do_class_restart}},
        {"class_start",             {1,     1,    do_class_start}},
        {"class_stop",              {1,     1,    do_class_stop}},
        {"copy",                    {2,     2,    do_copy}},
        {"domainname",              {1,     1,    do_domainname}},
        {"enable",                  {1,     1,    do_enable}},
        {"exec",                    {1,     kMax, do_exec}},
        {"exec_start",              {1,     1,    do_exec_start}},
        {"export",                  {2,     2,    do_export}},
        {"hostname",                {1,     1,    do_hostname}},
        {"ifup",                    {1,     1,    do_ifup}},
        {"init_user0",              {0,     0,    do_init_user0}},
        {"insmod",                  {1,     kMax, do_insmod}},
        {"installkey",              {1,     1,    do_installkey}},
        {"load_persist_props",      {0,     0,    do_load_persist_props}},
        {"load_system_props",       {0,     0,    do_load_system_props}},
        {"loglevel",                {1,     1,    do_loglevel}},
        {"mkdir",                   {1,     4,    do_mkdir}},
        {"mount_all",               {1,     kMax, do_mount_all}},
        {"mount",                   {3,     kMax, do_mount}},
        {"umount",                  {1,     1,    do_umount}},
        {"restart",                 {1,     1,    do_restart}},
        {"restorecon",              {1,     kMax, do_restorecon}},
        {"restorecon_recursive",    {1,     kMax, do_restorecon_recursive}},
        {"rm",                      {1,     1,    do_rm}},
        {"rmdir",                   {1,     1,    do_rmdir}},
        {"setprop",                 {2,     2,    do_setprop}},
        {"setrlimit",               {3,     3,    do_setrlimit}},
        {"start",                   {1,     1,    do_start}},
        {"stop",                    {1,     1,    do_stop}},
        {"swapon_all",              {1,     1,    do_swapon_all}},
        {"symlink",                 {2,     2,    do_symlink}},
        {"sysclktz",                {1,     1,    do_sysclktz}},
        {"trigger",                 {1,     1,    do_trigger}},
        {"verity_load_state",       {0,     0,    do_verity_load_state}},
        {"verity_update_state",     {0,     0,    do_verity_update_state}},
        {"wait",                    {1,     2,    do_wait}},
        {"wait_for_prop",           {2,     2,    do_wait_for_prop}},
        {"write",                   {2,     2,    do_write}},
    };
    // clang-format on
    return builtin_functions;
}

```

接下来我们看看EndSection,直接是调用ActionManager::GetInstance().AddAction

```C
void ActionParser::EndSection() {
    if (action_ && action_->NumCommands() > 0) {
        ActionManager::GetInstance().AddAction(std::move(action_));
    }
}
```

AddAction首先是查找是否有存在的同名Action，如果有就将他们的命令合并，没有就将它存入数组actions_中
```C
void ActionManager::AddAction(std::unique_ptr<Action> action) {
    auto old_action_it =
        std::find_if(actions_.begin(), actions_.end(),
                     [&action] (std::unique_ptr<Action>& a) {
                         return action->TriggersEqual(*a);
                     });//find_if是集合中用于比较的模板，上面这种写法是lambda表达式

    if (old_action_it != actions_.end()) {//在数组actions中找到Action说明已经存在同名，就合并command
        (*old_action_it)->CombineAction(*action);
    } else { //找不到就加入数组
        actions_.emplace_back(std::move(action));
    }
}

bool Action::TriggersEqual(const Action& other) const {
    return property_triggers_ == other.property_triggers_ &&
        event_trigger_ == other.event_trigger_;//比较之前记录的event trigger和property trigger
}

void Action::CombineAction(const Action& action) {
    for (const auto& c : action.commands_) { //将新的Action中的command合并到老的Action
        commands_.emplace_back(c);
    }
}
```

EndFile是一个空实现,定义在platform/system/core/init/action.h
```C
class ActionParser : public SectionParser {
public:
    ActionParser() : action_(nullptr) {
    }
    bool ParseSection(const std::vector<std::string>& args,
                      std::string* err) override;
    bool ParseLineSection(const std::vector<std::string>& args,
                          const std::string& filename, int line,
                          std::string* err) const override;
    void EndSection() override;
    void EndFile(const std::string&) override { //空实现
    }
private:
    std::unique_ptr<Action> action_;
};
```

讲了这么多，小结一下ActionParser做的事情. 它有三个重要的重载函数，ParseSection、ParseLineSection、EndSection.

- ParseSection函数的作用是构造一个Action对象，将trigger条件记录到Action这个对象中
- ParseLineSection作用是根据命令在一个map中找到对应的执行函数，然后将信息记录到之前构造的Action中
- EndSection作用是将前两步构造的Action存入一个数组中，存入之前比较下数组中是否已经存在同名的Action，如果有就合并command

### 2.4 ServiceParser
定义在platform/system/core/init/service.cpp

我们还是分析它的四个函数ParseSection、ParseLineSection、EndSection、EndFile

ParseSection首先是判断单词个数至少有三个，因为必须有一个服务名称和执行文件,然后是判断名称是否合法，主要是一些长度及内容的检查，最后就是构造一个Service对象
```C
bool ServiceParser::ParseSection(const std::vector<std::string>& args,
                                 std::string* err) {
    if (args.size() < 3) { // 传入单词个数至少三个
        *err = "services must have a name and a program";
        return false;
    }

    const std::string& name = args[1];
    if (!IsValidName(name)) {//检查名称是否合法
        *err = StringPrintf("invalid service name '%s'", name.c_str());
        return false;
    }

    std::vector<std::string> str_args(args.begin() + 2, args.end());
    service_ = std::make_unique<Service>(name, str_args);// 构造Service对象
    return true;
}
```

ParseLineSection直接执行Service的ParseLine函数
```C
bool ServiceParser::ParseLineSection(const std::vector<std::string>& args,
                                     const std::string& filename, int line,
                                     std::string* err) const {
    return service_ ? service_->ParseLine(args, err) : false;
}
```

ParseLine的思路跟之前Action一样，就是根据option名称从map中找到对应的执行函数,然后执行这个函数.
这些执行函数主要作用就是对传入参数做一些处理，然后将信息记录到Service对象中

```C
bool Service::ParseLine(const std::vector<std::string>& args, std::string* err) {
    if (args.empty()) {
        *err = "option needed, but not provided";
        return false;
    }

    static const OptionParserMap parser_map;
    auto parser = parser_map.FindFunction(args[0], args.size() - 1, err);//从map中找出执行函数

    if (!parser) {
        return false;
    }

    return (this->*parser)(args, err);//执行找到的这个函数
}
```

map()返回的map如下,定义在定义在platform/system/core/init/service.cpp中

```C
Service::OptionParserMap::Map& Service::OptionParserMap::map() const {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    // clang-format off
    static const Map option_parsers = {
        {"capabilities",
                        {1,     kMax, &Service::ParseCapabilities}},
        {"class",       {1,     kMax, &Service::ParseClass}},
        {"console",     {0,     1,    &Service::ParseConsole}},
        {"critical",    {0,     0,    &Service::ParseCritical}},
        {"disabled",    {0,     0,    &Service::ParseDisabled}},
        {"group",       {1,     NR_SVC_SUPP_GIDS + 1, &Service::ParseGroup}},
        {"ioprio",      {2,     2,    &Service::ParseIoprio}},
        {"priority",    {1,     1,    &Service::ParsePriority}},
        {"keycodes",    {1,     kMax, &Service::ParseKeycodes}},
        {"oneshot",     {0,     0,    &Service::ParseOneshot}},
        {"onrestart",   {1,     kMax, &Service::ParseOnrestart}},
        {"oom_score_adjust",
                        {1,     1,    &Service::ParseOomScoreAdjust}},
        {"namespace",   {1,     2,    &Service::ParseNamespace}},
        {"seclabel",    {1,     1,    &Service::ParseSeclabel}},
        {"setenv",      {2,     2,    &Service::ParseSetenv}},
        {"socket",      {3,     6,    &Service::ParseSocket}},
        {"file",        {2,     2,    &Service::ParseFile}},
        {"user",        {1,     1,    &Service::ParseUser}},
        {"writepid",    {1,     kMax, &Service::ParseWritepid}},
    };
    // clang-format on
    return option_parsers;
}
```

接下来我们看看EndSection，直接调用ServiceManager的AddService函数

```C
void ServiceParser::EndSection() {
    if (service_) {
        ServiceManager::GetInstance().AddService(std::move(service_));
    }
}
```

AddService的实现比较简单，就是通过比较service的name，查看存放Service的数组services_中是否有同名的service，如果有就打印下错误日志，直接返回，
如果不存在就加入数组中
```C
void ServiceManager::AddService(std::unique_ptr<Service> service) {
    Service* old_service = FindServiceByName(service->name()); //查找services_中是否已存在同名service
    if (old_service) {
        LOG(ERROR) << "ignored duplicate definition of service '" << service->name() << "'";
        return;
    }
    services_.emplace_back(std::move(service));//加入数组
}

Service* ServiceManager::FindServiceByName(const std::string& name) const {
    auto svc = std::find_if(services_.begin(), services_.end(),
                            [&name] (const std::unique_ptr<Service>& s) {
                                return name == s->name();
                            });//跟之前action一样，遍历数组进行比较，查找同名service
    if (svc != services_.end()) {
        return svc->get(); //找到就返回service
    }
    return nullptr;
}
```

EndFile依然是一个空实现,定义在platform/system/core/init/service.h
```C
class ServiceParser : public SectionParser {
public:
    ServiceParser() : service_(nullptr) {
    }
    bool ParseSection(const std::vector<std::string>& args,
                      std::string* err) override;
    bool ParseLineSection(const std::vector<std::string>& args,
                          const std::string& filename, int line,
                          std::string* err) const override;
    void EndSection() override;
    void EndFile(const std::string&) override { //空实现
    }
private:
    bool IsValidName(const std::string& name) const;

    std::unique_ptr<Service> service_;
};
```

从上面可以看出，ServiceParser的处理跟ActionParser差不多，区别在于Action将执行函数存起来等待Trigger触发时执行，Service找到执行函数后是马上执行

### 2.4 ImportParser
定义在platform/system/core/init/import_parser.cpp

最后我们看看ImportParser，ImportParser的ParseLineSection、EndSection都是空实现，只实现了ParseSection和EndFile,
因为它的语法比较单一，只有一行. 我们来看看它的ParseSection函数

首先检查单词只能是两个，因为只能是import xxx 这种语法，然后调用expand_props处理下参数，最后将结果放入数组imports_存起来
```C
bool ImportParser::ParseSection(const std::vector<std::string>& args,
                                std::string* err) {
    if (args.size() != 2) { //检查参数只能是两个
        *err = "single argument needed for import\n";
        return false;
    }

    std::string conf_file;
    bool ret = expand_props(args[1], &conf_file); //处理第二个参数
    if (!ret) {
        *err = "error while expanding import";
        return false;
    }

    LOG(INFO) << "Added '" << conf_file << "' to import list";
    imports_.emplace_back(std::move(conf_file)); //存入数组
    return true;
}
```

expand_props 定义在platform/system/core/init/util.cpp ，主要作用就是找到${x.y}或$x.y这种语法，将x.y取出来作为name，去属性系统中找对应的value，然后替换

```C
bool expand_props(const std::string& src, std::string* dst) {
    const char* src_ptr = src.c_str();

    if (!dst) {
        return false;
    }

    /* - variables can either be $x.y or ${x.y}, in case they are only part
     *   of the string.
     * - will accept $$ as a literal $.
     * - no nested property expansion, i.e. ${foo.${bar}} is not supported,
     *   bad things will happen
     * - ${x.y:-default} will return default value if property empty.
     */
    //这段英文大概的意思是 参数要么是$x.y，要么是${x.y}，它们都是路径的一部分，$$表示字符 $ ,
    //${foo.${bar}}这种递归写法是不支持的，因为会发生一些糟糕的事情
    //${x.y:-default}会将default作为默认值返回，如果找不到对应的属性值的话
    while (*src_ptr) {
        const char* c;

        c = strchr(src_ptr, '$');
        if (!c) { // 找不到$符号，直接将dst赋值为src返回
            dst->append(src_ptr);
            return true;
        }

        dst->append(src_ptr, c);
        c++;

        if (*c == '$') { //跳过$
            dst->push_back(*(c++));
            src_ptr = c;
            continue;
        } else if (*c == '\0') {
            return true;
        }

        std::string prop_name;
        std::string def_val;
        if (*c == '{') { //找到 { 就准备找 }的下标，然后截取它们之间的字符串，对应${x.y}的情况
            c++;
            const char* end = strchr(c, '}');
            if (!end) {
                // failed to find closing brace, abort.
                LOG(ERROR) << "unexpected end of string in '" << src << "', looking for }";
                return false;
            }
            prop_name = std::string(c, end); //截取{}之间的字符串作为name
            c = end + 1;
            size_t def = prop_name.find(":-"); //如果发现有 ":-" ,就将后面的值作为默认值先存起来
            if (def < prop_name.size()) {
                def_val = prop_name.substr(def + 2);
                prop_name = prop_name.substr(0, def);
            }
        } else { //对应$x.y的情况
            prop_name = c;
            LOG(ERROR) << "using deprecated syntax for specifying property '" << c << "', use ${name} instead";
            c += prop_name.size();
        }

        if (prop_name.empty()) {
            LOG(ERROR) << "invalid zero-length property name in '" << src << "'";
            return false;
        }

        std::string prop_val = android::base::GetProperty(prop_name, ""); //通过name在属性系统中找对应的value，内部调用的是之前属性系统的__system_property_find函数
        if (prop_val.empty()) { //没有找到值就返回默认值
            if (def_val.empty()) {
                LOG(ERROR) << "property '" << prop_name << "' doesn't exist while expanding '" << src << "'";
                return false;
            }
            prop_val = def_val;
        }

        dst->append(prop_val);
        src_ptr = c;
    }

    return true;
}
```

EndFile的实现比较简单，就是复制下ParseSection函数解析的.rc文件数组，然后遍历数组，调用最开始的ParseConfig函数解析一个完整的路径

```C
void ImportParser::EndFile(const std::string& filename) {
    auto current_imports = std::move(imports_);
    imports_.clear();
    for (const auto& s : current_imports) {
        if (!Parser::GetInstance().ParseConfig(s)) {
            PLOG(ERROR) << "could not import file '" << s << "' from '" << filename << "'";
        }
    }
}
```

由此，我们将Android Init Language语法的转化过程分析完毕，其实它们核心的解析器就三个，ActionParser,ServiceParser，ImportParser.
而这几个解析器主要是实现ParseSection、ParseLineSection、EndSection、EndFile四个函数
- ParseSection用于解析Section的第一行，比如
```C
on early
service ueventd /sbin/ueventd
import /init.${ro.zygote}.rc
```
- ParseLineSection用于解析Section的command或option,比如
```C
write /proc/1/oom_score_adj -1000
class core
```
- EndSection用于处理Action和Service同名的情况，以及将解析的对象存入数组备用
- EndFile只有在ImportParser中有用到，主要是解析导入的.rc文件

## 三、加入一些事件和一些Action

经过上一步的解析，系统已经准备就绪，是时候触发Trigger了
```C
    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) parser.DumpState(); //打印一些当前Parser的信息，默认是不执行的

    ActionManager& am = ActionManager::GetInstance();

    am.QueueEventTrigger("early-init");//QueueEventTrigger用于触发Action,这里触发 early-init事件

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
```

### 3.1 QueueEventTrigger
定义在platform/system/core/init/action.cpp

它并没有去触发trigger，而是构造了一个EventTrigger对象，放到队列中存起来
```C
void ActionManager::QueueEventTrigger(const std::string& trigger) {
    trigger_queue_.push(std::make_unique<EventTrigger>(trigger));
}

class EventTrigger : public Trigger {
public:
    explicit EventTrigger(const std::string& trigger) : trigger_(trigger) {
    }
    bool CheckTriggers(const Action& action) const override {
        return action.CheckEventTrigger(trigger_);
    }
private:
    const std::string trigger_;
};
```

### 3.2 QueueBuiltinAction
定义在platform/system/core/init/action.cpp

这个函数有两个参数，第一个参数是一个函数指针，第二参数是字符串. 首先是创建一个Action对象，将第二参数作为Action触发条件，
将第一个参数作为Action触发后的执行命令，并且又把第二个参数作为命令的参数，最后是将Action加入触发队列并加入Action列表
```C
void ActionManager::QueueBuiltinAction(BuiltinFunction func,
                                       const std::string& name) {
    auto action = std::make_unique<Action>(true);
    std::vector<std::string> name_vector{name};

    if (!action->InitSingleTrigger(name)) { //调用InitTriggers，之前讲过用于将name加入Action的trigger列表
        return;
    }

    action->AddCommand(func, name_vector);//加入Action的command列表

    trigger_queue_.push(std::make_unique<BuiltinTrigger>(action.get()));//将Action加入触发队列
    actions_.emplace_back(std::move(action));//加入Action列表
}
```


## 四、触发所有事件并不断监听新的事件

之前的所有工作都是往各种数组、队列里面存入信息，并没有真正去触发，而接下来的工作就是真正去触发这些事件，以及用epoll不断监听新的事件

```C
    while (true) {
        // By default, sleep until something happens.
        int epoll_timeout_ms = -1; //epoll超时时间，相当于阻塞时间

         /*
          * 1.waiting_for_prop和IsWaitingForExec都是判断一个Timer为不为空，相当于一个标志位
          * 2.waiting_for_prop负责属性设置，IsWaitingForExe负责service运行
          * 3.当有属性设置或Service开始运行时，这两个值就不为空，直到执行完毕才置为空
          * 4.其实这两个判断条件主要作用就是保证属性设置和service启动的完整性，也可以说是为了同步
          */
        if (!(waiting_for_prop || ServiceManager::GetInstance().IsWaitingForExec())) {
            am.ExecuteOneCommand(); //执行一个command
        }
        if (!(waiting_for_prop || ServiceManager::GetInstance().IsWaitingForExec())) {
            restart_processes(); //重启服务

            // If there's a process that needs restarting, wake up in time for that.
            if (process_needs_restart_at != 0) { //当有进程需要重启时，设置epoll_timeout_ms为重启等待时间
                epoll_timeout_ms = (process_needs_restart_at - time(nullptr)) * 1000;
                if (epoll_timeout_ms < 0) epoll_timeout_ms = 0;
            }

            // If there's more work to do, wake up again immediately.
            if (am.HasMoreCommands()) epoll_timeout_ms = 0; //当还有命令要执行时，将epoll_timeout_ms设置为0
        }

        epoll_event ev;
        /*
         * 1.epoll_wait与上一篇中讲的epoll_create1、epoll_ctl是一起使用的
         * 2.epoll_create1用于创建epoll的文件描述符，epoll_ctl、epoll_wait都把它创建的fd作为第一个参数传入
         * 3.epoll_ctl用于操作epoll，EPOLL_CTL_ADD：注册新的fd到epfd中，EPOLL_CTL_MOD：修改已经注册的fd的监听事件，EPOLL_CTL_DEL：从epfd中删除一个fd；
         * 4.epoll_wait用于等待事件的产生，epoll_ctl调用EPOLL_CTL_ADD时会传入需要监听什么类型的事件，
         *   比如EPOLLIN表示监听fd可读，当该fd有可读的数据时，调用epoll_wait经过epoll_timeout_ms时间就会把该事件的信息返回给&ev
         */
        int nr = TEMP_FAILURE_RETRY(epoll_wait(epoll_fd, &ev, 1, epoll_timeout_ms));
        if (nr == -1) {
            PLOG(ERROR) << "epoll_wait failed";
        } else if (nr == 1) {
            ((void (*)()) ev.data.ptr)();//当有event返回时，取出ev.data.ptr（之前epoll_ctl注册时的回调函数），直接执行
            //上一篇中在signal_handler_init和start_property_service有注册两个fd的监听，一个用于监听SIGCHLD(子进程结束信号)，一个用于监听属性设置
        }
    }

    return 0;
}
```

### 4.1 ExecuteOneCommand
定义在platform/system/core/init/action.cpp

从名字可以看出，它只执行一个command，是的，只执行一个. 在函数一开始就从trigger_queue_队列中取出一个trigger，
然后遍历所有action，找出满足trigger条件的action加入待执行列表current_executing_actions_中，
接着从这个列表中取出一个action，执行它的第一个命令，并将命令所在下标自加1. 由于ExecuteOneCommand外部是一个无限循环，
因此按照上面的逻辑一遍遍执行，将按照trigger表的顺序，依次执行满足trigger条件的action，然后依次执行action中的命令.


```C
void ActionManager::ExecuteOneCommand() {
    // Loop through the trigger queue until we have an action to execute
    while (current_executing_actions_.empty() && !trigger_queue_.empty()) {//current_executing_actions_.empty保证了一次只遍历一个trigger
        for (const auto& action : actions_) {//遍历所有的Action
            if (trigger_queue_.front()->CheckTriggers(*action)) {//满足当前Trigger条件的就加入队列current_executing_actions_
                current_executing_actions_.emplace(action.get());
            }
        }
        trigger_queue_.pop();//从trigger_queue_中踢除一个trigger
    }

    if (current_executing_actions_.empty()) {
        return;
    }

    auto action = current_executing_actions_.front();//从满足trigger条件的action队列中取出一个action

    if (current_command_ == 0) {
        std::string trigger_name = action->BuildTriggersString();
        LOG(INFO) << "processing action (" << trigger_name << ")";
    }

    action->ExecuteOneCommand(current_command_);//执行该action中的第current_command_个命令

    // If this was the last command in the current action, then remove
    // the action from the executing list.
    // If this action was oneshot, then also remove it from actions_.
    ++current_command_; //下标加1
    if (current_command_ == action->NumCommands()) { //如果是最后一条命令
        current_executing_actions_.pop();//将该action从current_executing_actions_中踢除
        current_command_ = 0;
        if (action->oneshot()) {//如果action只执行一次，将该action从数组actions_中踢除
            auto eraser = [&action] (std::unique_ptr<Action>& a) {
                return a.get() == action;
            };
            actions_.erase(std::remove_if(actions_.begin(), actions_.end(), eraser));
        }
    }
}
```

### 4.1 restart_processes
定义在platform/system/core/init/init.cpp

restart_processes调用的其实是ForEachServiceWithFlags函数，这个函数主要是遍历services_数组，比较它们的flags是否是SVC_RESTARTING，
也就是当前service是否是等待重启的，如果是就执行它的RestartIfNeeded函数

```C
static void restart_processes()
{
    process_needs_restart_at = 0;
    ServiceManager::GetInstance().ForEachServiceWithFlags(SVC_RESTARTING, [](Service* s) {
        s->RestartIfNeeded(&process_needs_restart_at);
    });
}

void ServiceManager::ForEachServiceWithFlags(unsigned matchflags,
                                             void (*func)(Service* svc)) const {
    for (const auto& s : services_) { //遍历所有service
        if (s->flags() & matchflags) {//找出flags是SVC_RESTARTING的，执行func，也就是传入的RestartIfNeeded
            func(s.get());
        }
    }
}
```

### 4.2 RestartIfNeeded
定义在platform/system/core/init/service.cpp

这个函数将主要工作交给了Start，也就是具体的启动service，但是交给它之前做了一些判断，也就是5秒内只能启动一个服务，
如果有多个服务，那么后续的服务将进入等待

```C
void Service::RestartIfNeeded(time_t* process_needs_restart_at) {
    boot_clock::time_point now = boot_clock::now();
    boot_clock::time_point next_start = time_started_ + 5s; //time_started_是上一个service启动的时间戳
    if (now > next_start) { //也就是说两个服务进程启动的间隔必须大于5s
        flags_ &= (~SVC_RESTARTING); // &= 加 ～ 相当于取消标记
        Start();
        return;
    }

    time_t next_start_time_t = time(nullptr) +
        time_t(std::chrono::duration_cast<std::chrono::seconds>(next_start - now).count());
    if (next_start_time_t < *process_needs_restart_at || *process_needs_restart_at == 0) {
        *process_needs_restart_at = next_start_time_t;//如果两个service启动间隔小于5s，将剩余时间赋值给process_needs_restart_at
    }
}
```

### 4.2 Start
定义在platform/system/core/init/service.cpp

Start是具体去启动服务了，它主要是调用clone或fork创建子进程，然后调用execve执行配置的二进制文件，另外根据之前在.rc文件中的配置，去执行这些配置

```C
bool Service::Start() {

    ... //清空标记，根据service的配置初始化console、SELinux策略等

    LOG(INFO) << "starting service '" << name_ << "'...";

    pid_t pid = -1;
    if (namespace_flags_) {//这个标记当service定义了namespace时会赋值为CLONE_NEWPID|CLONE_NEWNS
        pid = clone(nullptr, nullptr, namespace_flags_ | SIGCHLD, nullptr); //以clone方式在新的namespace创建子进程
    } else {
        pid = fork();//以fork方式创建子进程
    }

    if (pid == 0) {//表示创建子进程成功

        ... //执行service配置的其他参数，比如setenv、writepid等

        std::vector<char*> strs;
        ExpandArgs(args_, &strs);//将args_解析一下，比如有${x.y}，然后赋值表strs
        if (execve(strs[0], (char**) &strs[0], (char**) ENV) < 0) { //执行系统调用execve，也就是执行配置的二进制文件，把参数传进去
            PLOG(ERROR) << "cannot execve('" << strs[0] << "')";
        }

        _exit(127);
    }

    if (pid < 0) { //子进程创建失败
        PLOG(ERROR) << "failed to fork for '" << name_ << "'";
        pid_ = 0;
        return false;
    }

    ... //执行service其他参数如oom_score_adjust_，改变service运行状态等
}
```

**小结**

这一阶段Init进程做了许多重要的事情，比如解析.rc文件，这里配置了所有需要执行的action和需要启动的service,
Init进程根据语法一步步去解析.rc，将这些配置转换成一个个数组、队列，然后开启无限循环去处理这些数组、队列中的command和service，并且通过epoll监听子进程结束和属性设置.

至此，我已经将Init进程的三个阶段讲解完了，下一篇我将讲解.rc中配置的一个重要的service--zygote,它是我们app程序的鼻祖.