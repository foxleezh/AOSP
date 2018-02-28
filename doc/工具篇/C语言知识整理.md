## 前言
上一篇中讲到，Linux系统执行完初始化操作最后会执行根目录下的init文件，init是一个可执行程序，它的源码在platform/system/core/init/init.cpp，本篇主要讲init进程相关的一些东西,主要讲解以下内容

- init进程第一阶段
- init进程第二阶段
- init.rc文件解析


本文涉及到的文件
```
platform/system/core/init/init.cpp
platform/system/core/init/log.cpp
```

## 一、init进程第一阶段

之前我们讲过init进程是用户空间的第一个进程,我们熟悉的app应用程序都是以它为父进程的,init进程入口函数是main函数,我们先看看第一阶段做了些什么

### 1.1 ueventd/watchdogd跳转及环境变量设置


```C
/*
 * 1.C++中主函数有两个参数，第一个参数argc表示参数个数，第二个参数是参数列表，也就是具体的参数
 * 2.init的main函数有两个其它入口，一是参数中有ueventd，进入ueventd_main,二是参数中有watchdogd，进入watchdogd_main
 */
int main(int argc, char** argv) {

    /*
     * 1.strcmp是String的一个函数，比较字符串，相等返回0
     * 2.C++中0也可以表示false
     * 3.basename是C库中的一个函数，得到特定的路径中的最后一个'/'后面的内容，
     * 比如/sdcard/miui_recovery/backup，得到的结果是backup
     */
    if (!strcmp(basename(argv[0]), "ueventd")) { //当argv[0]的内容为ueventd时，strcmp的值为0,！strcmp为1
    //1表示true，也就执行ueventd_main,ueventd主要是负责设备节点的创建、权限设定等一些列工作
        return ueventd_main(argc, argv);
    }

    if (!strcmp(basename(argv[0]), "watchdogd")) {//watchdogd俗称看门狗，用于系统出问题时重启系统 
        return watchdogd_main(argc, argv);
    }

    if (REBOOT_BOOTLOADER_ON_PANIC) {
        install_reboot_signal_handlers(); //初始化重启系统的处理信号，内部通过sigaction 注册信号，当监听到该信号时重启系统
    }

    add_environment("PATH", _PATH_DEFPATH);//注册环境变量PATH
    //#define	_PATH_DEFPATH	"/sbin:/system/sbin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin"
       
```

#### 1.1.1 ueventd_main
定义在platform/system/core/init/ueventd.cpp

Android根文件系统的映像中不存在“/dev”目录，该目录是init进程启动后动态创建的。
因此，建立Android中设备节点文件的重任，也落在了init进程身上。为此，init进程创建子进程ueventd，并将创建设备节点文件的工作托付给ueventd。
ueventd通过两种方式创建设备节点文件。
第一种方式对应“冷插拔”（Cold Plug），即以预先定义的设备信息为基础，当ueventd启动后，统一创建设备节点文件。这一类设备节点文件也被称为静态节点文件。
第二种方式对应“热插拔”（Hot Plug），即在系统运行中，当有设备插入USB端口时，ueventd就会接收到这一事件，为插入的设备动态创建设备节点文件。这一类设备节点文件也被称为动态节点文件。

```C
int ueventd_main(int argc, char **argv)
{
    /*
     * init sets the umask to 077 for forked processes. We need to
     * create files with exact permissions, without modification by
     * the umask.
     */
    umask(000); //设置新建文件的默认值,这个与chmod相反,这里相当于新建文件后的权限为666

    /* Prevent fire-and-forget children from becoming zombies.
     * If we should need to wait() for some children in the future
     * (as opposed to none right now), double-forking here instead
     * of ignoring SIGCHLD may be the better solution.
     */
    signal(SIGCHLD, SIG_IGN);//忽略子进程终止信号

    InitKernelLogging(argv); //初始化日志输出

    LOG(INFO) << "ueventd started!";

    selinux_callback cb;
    cb.func_log = selinux_klog_callback;
    selinux_set_callback(SELINUX_CB_LOG, cb);//注册selinux相关的用于打印log的回调函数

    ueventd_parse_config_file("/ueventd.rc"); //解析.rc文件,这个后文再讲
    ueventd_parse_config_file("/vendor/ueventd.rc");
    ueventd_parse_config_file("/odm/ueventd.rc");

    /*
     * keep the current product name base configuration so
     * we remain backwards compatible and allow it to override
     * everything
     * TODO: cleanup platform ueventd.rc to remove vendor specific
     * device node entries (b/34968103)
     */
    std::string hardware = android::base::GetProperty("ro.hardware", "");
    ueventd_parse_config_file(android::base::StringPrintf("/ueventd.%s.rc", hardware.c_str()).c_str());

    device_init();//创建一个socket来接收uevent，再对内核启动时注册到/sys/下的驱动程序进行“冷插拔”处理，以创建对应的节点文件。

    pollfd ufd;
    ufd.events = POLLIN;
    ufd.fd = get_device_fd();//获取device_init中创建出的socket

    while (true) {//开户无限循环，随时监听驱动
        ufd.revents = 0;
        int nr = poll(&ufd, 1, -1);//监听来自驱动的uevent
        if (nr <= 0) {
            continue;
        }
        if (ufd.revents & POLLIN) {
            handle_device_fd();//驱动程序进行“热插拔”处理，以创建对应的节点文件。
        }
    }

    return 0;
}

```

#### 1.1.2 watchdogd_main
定义在platform/system/core/init/watchdogd.cpp

"看门狗"本身是一个定时器电路，内部会不断的进行计时（或计数）操作
计算机系统和"看门狗"有两个引脚相连接，正常运行时每隔一段时间就会通过其中一个引脚向"看门狗"发送信号，"看门狗"接收到信号后会将计时器清零并重新开始计时
而一旦系统出现问题，进入死循环或任何阻塞状态，不能及时发送信号让"看门狗"的计时器清零，当计时结束时，"看门狗"就会通过另一个引脚向系统发送“复位信号”，让系统重启

watchdogd_main主要是定时器作用,而DEV_NAME就是那个引脚
```C
int watchdogd_main(int argc, char **argv) {
    InitKernelLogging(argv);

    int interval = 10;
    /*
     * C++中atoi作用是将字符串转变为数值
     */
    if (argc >= 2) interval = atoi(argv[1]);

    int margin = 10;
    if (argc >= 3) margin = atoi(argv[2]);

    LOG(INFO) << "watchdogd started (interval " << interval << ", margin " << margin << ")!";

    int fd = open(DEV_NAME, O_RDWR|O_CLOEXEC); //打开文件 /dev/watchdog
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << DEV_NAME;
        return 1;
    }

    int timeout = interval + margin;
    /*
     * ioctl是设备驱动程序中对设备的I/O通道进行管理的函数,WDIOC_SETTIMEOUT是设置超时时间
     */
    int ret = ioctl(fd, WDIOC_SETTIMEOUT, &timeout);
    if (ret) {
        PLOG(ERROR) << "Failed to set timeout to " << timeout;
        ret = ioctl(fd, WDIOC_GETTIMEOUT, &timeout);
        if (ret) {
            PLOG(ERROR) << "Failed to get timeout";
        } else {
            if (timeout > margin) {
                interval = timeout - margin;
            } else {
                interval = 1;
            }
            LOG(WARNING) << "Adjusted interval to timeout returned by driver: "
                         << "timeout " << timeout
                         << ", interval " << interval
                         << ", margin " << margin;
        }
    }

    while (true) {//每间隔一定时间往文件中写入一个空字符,这就是看门狗的关键了
        write(fd, "", 1);
        sleep(interval);
    }
}
```
#### 1.1.3 install_reboot_signal_handlers
定义在platform/system/core/init/init.cpp

这个函数主要作用将各种信号量，如SIGABRT,SIGBUS等的行为设置为SA_RESTART,一旦监听到这些信号即执行重启系统

```C
static void install_reboot_signal_handlers() {
    // Instead of panic'ing the kernel as is the default behavior when init crashes,
    // we prefer to reboot to bootloader on development builds, as this will prevent
    // boot looping bad configurations and allow both developers and test farms to easily
    // recover.
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigfillset(&action.sa_mask);//将所有信号加入至信号集
    action.sa_handler = [](int) {
        // panic() reboots to bootloader
        panic(); //重启系统
    };
    action.sa_flags = SA_RESTART;
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGSEGV, &action, nullptr);
#if defined(SIGSTKFLT)
    sigaction(SIGSTKFLT, &action, nullptr);
#endif
    sigaction(SIGSYS, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);
}
```

#### 1.1.4 add_environment
定义在platform/system/core/init/init.cpp

这个函数主要作用是将一个键值对放到一个Char数组中,如果数组中有key就替换,没有就插入,跟Java中的Map差不多

```C
/* add_environment - add "key=value" to the current environment */
int add_environment(const char *key, const char *val)
{
    size_t n;
    size_t key_len = strlen(key);

    /* The last environment entry is reserved to terminate the list */
    for (n = 0; n < (arraysize(ENV) - 1); n++) {

        /* Delete any existing entry for this key */
        if (ENV[n] != NULL) {
        /*
         * C++中strcspn用于返回字符所在下标,相当于String的indexof
         */
            size_t entry_key_len = strcspn(ENV[n], "=");
            if ((entry_key_len == key_len) && (strncmp(ENV[n], key, entry_key_len) == 0)) { //如果key相同,删除对应数据
                free((char*)ENV[n]);
                ENV[n] = NULL;
            }
        }

        /* Add entry if a free slot is available */
        if (ENV[n] == NULL) { //如果没有对应key,则插入数据
            char* entry;
            asprintf(&entry, "%s=%s", key, val);
            ENV[n] = entry;
            return 0;
        }
    }

    LOG(ERROR) << "No env. room to store: '" << key << "':'" << val << "'";

    return -1;
}
```

### 1.2 挂载文件系统并创建目录


```C

    bool is_first_stage = (getenv("INIT_SECOND_STAGE") == nullptr);//查看是否有环境变量INIT_SECOND_STAGE

    /*
     * 1.init的main方法会执行两次，由is_first_stage控制,first_stage就是第一阶段要做的事
     */
    if (is_first_stage) {//只执行一次，因为在方法体中有设置INIT_SECOND_STAGE
        boot_clock::time_point start_time = boot_clock::now();

        // Clear the umask.
        umask(0); //清空文件权限

        // Get the basic filesystem setup we need put together in the initramdisk
        // on / and then we'll let the rc file figure out the rest.
        mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
        mkdir("/dev/pts", 0755);
        mkdir("/dev/socket", 0755);
        mount("devpts", "/dev/pts", "devpts", 0, NULL);
        #define MAKE_STR(x) __STRING(x)
        mount("proc", "/proc", "proc", 0, "hidepid=2,gid=" MAKE_STR(AID_READPROC));
        // Don't expose the raw commandline to unprivileged processes.
        chmod("/proc/cmdline", 0440);
        gid_t groups[] = { AID_READPROC };
        setgroups(arraysize(groups), groups);
        mount("sysfs", "/sys", "sysfs", 0, NULL);
        mount("selinuxfs", "/sys/fs/selinux", "selinuxfs", 0, NULL);
        mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
        mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
        mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));

        ...

    }

   ...

    
}

 
```

#### 1.2.1 mount
mount是用来挂载文件系统的，mount属于Linux系统调用
```C
int mount(const char *source, const char *target, const char *filesystemtype, 
unsigned long mountflags, const void *data);
```
参数：
source：将要挂上的文件系统，通常是一个设备名。
target：文件系统所要挂载的目标目录。
filesystemtype：文件系统的类型，可以是"ext2"，"msdos"，"proc"，"ntfs"，"iso9660"。。。
mountflags：指定文件系统的读写访问标志，可能值有以下

|参数|含义|
| :-- | :-- |
| MS_BIND| 执行bind挂载，使文件或者子目录树在文件系统内的另一个点上可视。| 
| MS_DIRSYNC| 同步目录的更新。| 
| MS_MANDLOCK| 允许在文件上执行强制锁。| 
| MS_MOVE| 移动子目录树。| 
| MS_NOATIME| 不要更新文件上的访问时间。| 
| MS_NODEV| 不允许访问设备文件。| 
| MS_NODIRATIME| 不允许更新目录上的访问时间。| 
| MS_NOEXEC| 不允许在挂上的文件系统上执行程序。| 
| MS_NOSUID| 执行程序时，不遵照set-user-ID和set-group-ID位。| 
| MS_RDONLY| 指定文件系统为只读。| 
| MS_REMOUNT| 重新加载文件系统。这允许你改变现存文件系统的mountflag和数据，而无需使用先卸载，再挂上文件系统的方式。| 
| MS_SYNCHRONOUS| 同步文件的更新。| 
| MNT_FORCE| 强制卸载，即使文件系统处于忙状态。| 
| MNT_EXPIRE| 将挂载点标记为过时。| 

data：文件系统特有的参数

在init初始化过程中，Android分别挂载了tmpfs，devpts，proc，sysfs这4类文件系统。

tmpfs是一种虚拟内存文件系统，它会将所有的文件存储在虚拟内存中， 
如果你将tmpfs文件系统卸载后，那么其下的所有的内容将不复存在。 
tmpfs既可以使用RAM，也可以使用交换分区，会根据你的实际需要而改变大小。 
tmpfs的速度非常惊人，毕竟它是驻留在RAM中的，即使用了交换分区，性能仍然非常卓越。 
由于tmpfs是驻留在RAM的，因此它的内容是不持久的。 
断电后，tmpfs的内容就消失了，这也是被称作tmpfs的根本原因。

devpts文件系统为伪终端提供了一个标准接口，它的标准挂接点是/dev/ pts。 
只要pty的主复合设备/dev/ptmx被打开，就会在/dev/pts下动态的创建一个新的pty设备文件。

proc文件系统是一个非常重要的虚拟文件系统，它可以看作是内核内部数据结构的接口， 
通过它我们可以获得系统的信息，同时也能够在运行时修改特定的内核参数。

与proc文件系统类似，sysfs文件系统也是一个不占有任何磁盘空间的虚拟文件系统。 
它通常被挂接在/sys目录下。sysfs文件系统是Linux2.6内核引入的， 
它把连接在系统上的设备和总线组织成为一个分级的文件，使得它们可以在用户空间存取
#### 1.2.2 mknod
mknod用于创建Linux中的设备文件
```
int mknod(const char* path, mode_t mode, dev_t dev) {
  
}
```

参数：
path：设备所在目录
mode：指定设备的类型和读写访问标志
可能的类型

|参数|含义|
| :-- | :-- |
|S_IFMT   |type of file ，文件类型掩码|
|S_IFREG  |regular 普通文件|
|S_IFBLK  |block special 块设备文件|
|S_IFDIR  |directory 目录文件|
|S_IFCHR  |character special 字符设备文件|
|S_IFIFO  |fifo 管道文件|
|S_IFNAM  |special named file 特殊文件|
|S_IFLNK  |symbolic link 链接文件|

dev 表示设备，由makedev(1, 9) 函数创建，9为主设备号、1为次设备号
#### 1.2.3 其他命令

mkdir也是Linux系统调用，作用是创建目录，第一个参数是目录路径，第二个是读写权限
chmod用于修改文件/目录的读写权限
setgroups 用来将list 数组中所标明的组加入到目前进程的组设置中

这里我解释下文件的权限，也就是类似0755这种，要理解权限首先要明白「用户和组」的概念
Linux系统可以有多个用户，多个用户可以属于同一个组，用户和组的概念就像我们人和家庭一样，人属于家庭的一分子，用户属于一个组，我们一般在Linux终端输入ls -al之后会有如下结果
```
drwxr-xr-x  7 foxleezh foxleezh   4096 2月  24 14:31 .android
```
第一个foxleezh表示所有者，这里的foxleezh表示一个用户，类似foxleezh这个人
第二个foxleezh表示文件所有用户组，这里的foxleezh表示一个组，类似foxleezh这个家庭
然后我们来看下dwxr-xr-x,这个要分成四部分来理解，d表示目录（文件用 - 表示），wxr表示所有者权限，xr表示文件所有用户组的权限，x表示其他用户的权限
w- 表示写权限，用2表示
x- 表示执行权限，用1表示
r- 表示读取权限，用4表示
那么dwxr-xr-x还有种表示方法就是751，是不是感觉跟0755差不多了，那0755前面那个0表示什么意思呢？
0755前面的0跟suid和guid有关
suid意味着其他用户拥有和文件所有者一样的权限，用4表示
guid意味着其他用户拥有和文件所有用户组一样的权限，用2表示

### 1.3 初始化日志输出、挂载分区设备


```C
 if (is_first_stage) {
       
          ...

        // Now that tmpfs is mounted on /dev and we have /dev/kmsg, we can actually
        // talk to the outside world...
        InitKernelLogging(argv);

        LOG(INFO) << "init first stage started!";

        if (!DoFirstStageMount()) {
            LOG(ERROR) << "Failed to mount required partitions early ...";
            panic();//重启系统
        }

        ...
    }
```
#### 1.3.1 InitKernelLogging
定义在platform/system/core/init/log.cpp

InitKernelLogging首先是将标准输入输出重定向到"/sys/fs/selinux/null"，然后设置log日志输出的等级
```C
void InitKernelLogging(char* argv[]) {
    // Make stdin/stdout/stderr all point to /dev/null.
    int fd = open("/sys/fs/selinux/null", O_RDWR); //打开文件
    if (fd == -1) {
        int saved_errno = errno;
        android::base::InitLogging(argv, &android::base::KernelLogger);
        errno = saved_errno;
        PLOG(FATAL) << "Couldn't open /sys/fs/selinux/null";
    }
    /*
     * dup2(int old_fd, int new_fd) 的作用是复制文件描述符，将old复制到new,下文中将
     *  0、1、2绑定到null设备上，通过标准的输入输出无法输出信息
     */
    dup2(fd, 0); //重定向标准输入stdin
    dup2(fd, 1);//重定向标准输出stdout 
    dup2(fd, 2);//重定向标准错误stderr
    if (fd > 2) close(fd);

    android::base::InitLogging(argv, &android::base::KernelLogger);//初始化log
}
```
#### 1.3.1 InitLogging
定义在platform/system/core/base/logging.cpp

```C
void InitLogging(char* argv[], LogFunction&& logger, AbortFunction&& aborter) {
/*
 * C++中foo(std::forward<T>(arg))表示将arg按原本的左值或右值，传递给foo方法，
   LogFunction& 这种表示是左值，LogFunction&&这种表示是右值
 */
  SetLogger(std::forward<LogFunction>(logger)); //设置logger处理函数
  SetAborter(std::forward<AbortFunction>(aborter));//设置aborter处理函数

  if (gInitialized) {
    return;
  }

  gInitialized = true;

  // Stash the command line for later use. We can use /proc/self/cmdline on
  // Linux to recover this, but we don't have that luxury on the Mac/Windows,
  // and there are a couple of argv[0] variants that are commonly used.
  if (argv != nullptr) {
    std::lock_guard<std::mutex> lock(LoggingLock());
    ProgramInvocationName() = basename(argv[0]);
  }

  const char* tags = getenv("ANDROID_LOG_TAGS");//获取系统当前日志输出等级
  if (tags == nullptr) {
    return;
  }

  std::vector<std::string> specs = Split(tags, " "); //将tags以空格拆分成数组
  for (size_t i = 0; i < specs.size(); ++i) {
    // "tag-pattern:[vdiwefs]"
    std::string spec(specs[i]);
    if (spec.size() == 3 && StartsWith(spec, "*:")) { //如果字符数为3且以*:开头
     //那么根据第三个字符来设置日志输出等级（比如*:d,就是DEBUG级别）
      switch (spec[2]) {
        case 'v':
          gMinimumLogSeverity = VERBOSE;
          continue;
        case 'd':
          gMinimumLogSeverity = DEBUG;
          continue;
        case 'i':
          gMinimumLogSeverity = INFO;
          continue;
        case 'w':
          gMinimumLogSeverity = WARNING;
          continue;
        case 'e':
          gMinimumLogSeverity = ERROR;
          continue;
        case 'f':
          gMinimumLogSeverity = FATAL_WITHOUT_ABORT;
          continue;
        // liblog will even suppress FATAL if you say 's' for silent, but that's
        // crazy!
        case 's':
          gMinimumLogSeverity = FATAL_WITHOUT_ABORT;
          continue;
      }
    }
    LOG(FATAL) << "unsupported '" << spec << "' in ANDROID_LOG_TAGS (" << tags
               << ")";
  }
}
```
```
 if (is_first_stage) {
       
          ...

        SetInitAvbVersionInRecovery();

        // Set up SELinux, loading the SELinux policy.
        selinux_initialize(true);//加载SELinux policy，也就是安全策略，
        //SELinux是「Security-Enhanced Linux」的简称，是美国国家安全局「NSA=The National Security Agency」
        //和SCC（Secure Computing Corporation）开发的 Linux的一个扩张强制访问控制安全模块。
        //在这种访问控制体系的限制下，进程只能访问那些在他的任务中所需要文件

        // We're in the kernel domain, so re-exec init to transition to the init domain now
        // that the SELinux policy has been loaded.

        /*
         * 1.这句英文大概意思是，我们执行第一遍时是在kernel domain，所以要重新执行init文件，切换到init domain，
         * 这样SELinux policy才已经加载进来了
         * 2.后面的security_failure函数会调用panic重启系统
         */
        if (restorecon("/init") == -1) { //restorecon命令用来恢复SELinux文件属性即恢复文件的安全上下文
            PLOG(ERROR) << "restorecon failed";
            security_failure();
        }

        ...
    }
```

```
 if (is_first_stage) {
       
          ...

        setenv("INIT_SECOND_STAGE", "true", 1);

        static constexpr uint32_t kNanosecondsPerMillisecond = 1e6;
        uint64_t start_ms = start_time.time_since_epoch().count() / kNanosecondsPerMillisecond;
        setenv("INIT_STARTED_AT", StringPrintf("%" PRIu64, start_ms).c_str(), 1);//记录第二阶段开始时间戳

        char* path = argv[0];
        char* args[] = { path, nullptr };
        execv(path, args); //重新执行main方法，进入第二阶段

        // execv() only returns if an error happened, in which case we
        // panic and never fall through this conditional.
        PLOG(ERROR) << "execv(\"" << path << "\") failed";
        security_failure();
    }
```


```
int main(int argc, char** argv) {

    ...
    
    // At this point we're in the second stage of init.
    InitKernelLogging(argv);
    LOG(INFO) << "init second stage started!";

    // Set up a session keyring that all processes will have access to. It
    // will hold things like FBE encryption keys. No process should override
    // its session keyring.
    keyctl(KEYCTL_GET_KEYRING_ID, KEY_SPEC_SESSION_KEYRING, 1); //初始化进程会话密钥

    // Indicate that booting is in progress to background fw loaders, etc.
    close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));//关闭/dev/.booting文件的相关权限

    property_init();//初始化属性，接下来的一系列操作都是从各个文件读取一些属性，然后通过property_set设置系统属性

    // If arguments are passed both on the command line and in DT,
    // properties set in DT always have priority over the command-line ones.
    /*
     * 1.这句英文的大概意思是，如果参数同时从命令行和DT传过来，DT的优先级总是大于命令行的
     * 2.DT即device-tree，中文意思是设备树，这里面记录自己的硬件配置和系统运行参数，参考http://www.wowotech.net/linux_kenrel/why-dt.html
     */
    process_kernel_dt();//处理DT属性
    process_kernel_cmdline();//处理命令行属性

    // Propagate the kernel variables to internal variables
    // used by init as well as the current required properties.
    export_kernel_boot_props();//处理其他的一些属性

    // Make the time that init started available for bootstat to log.
    property_set("ro.boottime.init", getenv("INIT_STARTED_AT"));
    property_set("ro.boottime.init.selinux", getenv("INIT_SELINUX_TOOK"));

    // Set libavb version for Framework-only OTA match in Treble build.
    const char* avb_version = getenv("INIT_AVB_VERSION");
    if (avb_version) property_set("ro.boot.avb_version", avb_version);

    // Clean up our environment.
    unsetenv("INIT_SECOND_STAGE"); //清空这些环境变量，因为之前都已经存入到系统属性中去了
    unsetenv("INIT_STARTED_AT");
    unsetenv("INIT_SELINUX_TOOK");
    unsetenv("INIT_AVB_VERSION");

    // Now set up SELinux for second stage.
    selinux_initialize(false); //第二阶段初始化SELinux policy
    selinux_restore_context();

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);//创建epoll实例，并返回epoll的文件描述符
    //EPOLL类似于POLL，是Linux特有的一种IO多路复用的机制，对于大量的描述符处理，EPOLL更有优势
    //epoll_create1是epoll_create的升级版，可以动态调整epoll实例中文件描述符的个数
    //EPOLL_CLOEXEC这个参数是为文件描述符添加O_CLOEXEC属性，参考http://blog.csdn.net/gqtcgq/article/details/48767691

    if (epoll_fd == -1) {
        PLOG(ERROR) << "epoll_create1 failed";
        exit(1);
    }

    signal_handler_init();//主要是创建handler处理子进程终止信号，创建一个匿名socket并注册到epoll进行监听

    property_load_boot_defaults();//从文件中加载一些属性，读取usb配置
    export_oem_lock_status();//设置ro.boot.flash.locked 属性
    start_property_service();//开启一个socket监听系统属性的设置
    set_usb_controller();//设置sys.usb.controller 属性
    
    ...

```


```
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