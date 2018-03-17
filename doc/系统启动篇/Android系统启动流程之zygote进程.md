## 前言

在上一篇中我们讲到，init进程会解析.rc文件，然后得到一些service去启动，这些service通常不是普通的服务，文档里面的称呼是daemon（守护进程）.
所谓守护进程就是这些服务进程会在系统初始化时启动，并一直运行于后台，直到系统关闭时终止. 我们本篇讲的zygote进程就是其中之一，zygote进程主要负责
创建Java虚拟机，加载系统资源，启动SystemServer进程，以及在后续运行过程中启动普通的应用程序. 

本文主要讲解以下内容

- zygote触发过程

本文涉及到的文件
```
platform/system/core/rootdir/init.zygoteXX.rc
platform/system/core/rootdir/init.rc
```

## 一、zygote触发过程

### 1.1 init.zygoteXX.rc
定义在platform/system/core/rootdir/init.zygoteXX.rc

我们知道service是定义在.rc文件中的，那么zygote定义在哪儿呢？在init.rc中有这样一句
```C
import /init.${ro.zygote}.rc
```
上节中讲到 ${ro.zygote} 会被替换成 ro.zyogte 的属性值，这个是由不同的硬件厂商自己定制的，
有四个值，zygote32、zygote64、zygote32_64、zygote64_32 ，也就是说可能有四种 .rc 文件，分别是：

- init.zygote32.rc：zygote 进程对应的执行程序是 app_process (纯 32bit 模式)
- init.zygote64.rc：zygote 进程对应的执行程序是 app_process64 (纯 64bit 模式)
- init.zygote32_64.rc：启动两个 zygote 进程 (名为 zygote 和 zygote_secondary)，对应的执行程序分别是 app_process32 (主模式)、app_process64
- init.zygote64_32.rc：启动两个 zygote 进程 (名为 zygote 和 zygote_secondary)，对应的执行程序分别是 app_process64 (主模式)、app_process32

为什么要定义这么多种情况呢？直接定义一个不就好了，这主要是因为Android 5.0以后开始支持64位程序，为了兼容32位和64位才这样定义.
不同的zygote.rc内容大致相同，主要区别体现在启动的是32位，还是64位的进程.
init.zygote32_64.rc和init.zygote64_32.rc会启动两个进程，且存在主次之分. 我们以init.zygote64_32.rc为例
```C
// 进程名称是zygote,运行的二进制文件在/system/bin/app_process64
// 启动参数是 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
service zygote /system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
    class main
    priority -20
    user root
    group root readproc
    socket zygote stream 660 root system //创建一个socket,名字叫zygote,以tcp形式
    onrestart write /sys/android_power/request_state wake //onrestart 指当进程重启时执行后面的命令
    onrestart write /sys/power/state on
    onrestart restart audioserver
    onrestart restart cameraserver
    onrestart restart media
    onrestart restart netd
    onrestart restart wificond
    writepid /dev/cpuset/foreground/tasks //创建子进程时，向/dev/cpuset/foreground/tasks 写入pid

// 另一个service ,名字 zygote_secondary
service zygote_secondary /system/bin/app_process32 -Xzygote /system/bin --zygote --socket-name=zygote_secondary --enable-lazy-preload
    class main
    priority -20
    user root
    group root readproc
    socket zygote_secondary stream 660 root system
    onrestart restart zygote
    writepid /dev/cpuset/foreground/tasks

```

### 1.2 start zygote
定义在 platform/system/core/rootdir/init.rc

定义了service，肯定有地方调用 start zygote ,搜索一下就在init.rc中找到了, 只要触发 zygote-start 就可以
```C
on zygote-start && property:ro.crypto.state=unencrypted
    # A/B update verifier that marks a successful boot.
    exec_start update_verifier_nonencrypted
    start netd
    start zygote
    start zygote_secondary

on zygote-start && property:ro.crypto.state=unsupported
    # A/B update verifier that marks a successful boot.
    exec_start update_verifier_nonencrypted
    start netd
    start zygote
    start zygote_secondary

on zygote-start && property:ro.crypto.state=encrypted && property:ro.crypto.type=file
    # A/B update verifier that marks a successful boot.
    exec_start update_verifier_nonencrypted
    start netd
    start zygote
    start zygote_secondary
```

zygote-start 是在 on late-init 中触发的

```C
on late-init
    ...

    trigger zygote-start
```

late-init 在哪儿触发的呢？其实上一篇中有讲到，在init进程的最后，会加入 late-init 的trigger
```C
    if (bootmode == "charger") {
        am.QueueEventTrigger("charger");
    } else {
        am.QueueEventTrigger("late-init");
    }
```

由此分析，zygote的触发是在init进程最后，接下来，我们看看start zygote是如何继续执行的.

### 1.3 app_processXX

上一篇中我们知道 start 命令有一个对应的执行函数 do_start ,定义在platform/system/core/init/builtins.cpp中

do_start首先是通过FindServiceByName去service数组中遍历，根据名字匹配出对应的service,然后调用service的Start函数，
Start函数我们在上一篇结尾有分析，主要是fork出一个新进程然后执行service对应的二进制文件，并将参数传递进去.
```C
static const Map builtin_functions = {
        ...

        {"start",                   {1,     1,    do_start}},

        ...
};

static int do_start(const std::vector<std::string>& args) {
    Service* svc = ServiceManager::GetInstance().FindServiceByName(args[1]); //找出对应service
    if (!svc) {
        LOG(ERROR) << "do_start: Service " << args[1] << " not found";
        return -1;
    }
    if (!svc->Start())
        return -1;
    return 0;
}

```

zygote对应的二进制文件是 /system/bin/app_process64 （以此为例），我们看一下对应的mk文件，
对应的目录在platform/frameworks/base/cmds/app_process/Android.mk,
其实不管是app_process、app_process32还是app_process64，对应的源文件都是app_main.cpp.

```C
...

app_process_src_files := \
    app_main.cpp \


LOCAL_SRC_FILES:= $(app_process_src_files)

...

LOCAL_MODULE:= app_process
LOCAL_MULTILIB := both
LOCAL_MODULE_STEM_32 := app_process32
LOCAL_MODULE_STEM_64 := app_process64

...

```

接下来，我们分析app_main.cpp.

## zygote参数解析

在app_main.cpp的main函数中，主要做的事情就是参数解析. 这个函数有两种启动模式：
- 一种是zygote模式，也就是初始化zygote进程，传递的参数有--start-system-server --socket-name=zygote，前者表示启动SystemServer，后者指定socket的名称
- 一种是application模式，也就是启动普通应用程序，传递的参数有class名字以及class带的参数

两者最终都是调用AppRuntime对象的start函数，加载ZygoteInit或RuntimeInit两个Java类，并将之前整理的参数传入进去

由于本篇讲的是zygote进程启动流程，因此接下来我将讲解ZygoteInit的加载.

```C
int main(int argc, char* const argv[])
{
    //将参数argv放到argv_String字符串中，然后打印出来
    //之前start zygote传入的参数是 -Xzygote /system/bin --zygote --start-system-server --socket-name=zygote
    if (!LOG_NDEBUG) {
      String8 argv_String;
      for (int i = 0; i < argc; ++i) {
        argv_String.append("\"");
        argv_String.append(argv[i]);
        argv_String.append("\" ");
      }
      ALOGV("app_process main with argv: %s", argv_String.string());
    }

    AppRuntime runtime(argv[0], computeArgBlockSize(argc, argv));//构建AppRuntime对象，并将参数传入
    // Process command line arguments
    // ignore argv[0]
    argc--;
    argv++;

    // Everything up to '--' or first non '-' arg goes to the vm.
    //
    // The first argument after the VM args is the "parent dir", which
    // is currently unused.
    //
    // After the parent dir, we expect one or more the following internal
    // arguments :
    //
    // --zygote : Start in zygote mode
    // --start-system-server : Start the system server.
    // --application : Start in application (stand alone, non zygote) mode.
    // --nice-name : The nice name for this process.
    //
    // For non zygote starts, these arguments will be followed by
    // the main class name. All remaining arguments are passed to
    // the main method of this class.
    //
    // For zygote starts, all remaining arguments are passed to the zygote.
    // main function.
    //
    // Note that we must copy argument string values since we will rewrite the
    // entire argument block when we apply the nice name to argv0.
    //
    // As an exception to the above rule, anything in "spaced commands"
    // goes to the vm even though it has a space in it.

    //上面这段英文大概讲的是，所有在 "--" 后面的非 "-"开头的参数都将传入vm, 但是有个例外是spaced commands数组中的参数

    const char* spaced_commands[] = { "-cp", "-classpath" };//这两个参数是Java程序需要依赖的Jar包，相当于import
    // Allow "spaced commands" to be succeeded by exactly 1 argument (regardless of -s).
    bool known_command = false;
    int i;
    for (i = 0; i < argc; i++) {
        if (known_command == true) { //将spaced_commands中的参数额外加入VM
          runtime.addOption(strdup(argv[i]));
          ALOGV("app_process main add known option '%s'", argv[i]);
          known_command = false;
          continue;
        }

        for (int j = 0;
             j < static_cast<int>(sizeof(spaced_commands) / sizeof(spaced_commands[0]));
             ++j) {
          if (strcmp(argv[i], spaced_commands[j]) == 0) {//比较参数是否是spaced_commands中的参数
            known_command = true;
            ALOGV("app_process main found known command '%s'", argv[i]);
          }
        }

        if (argv[i][0] != '-') { //如果参数第一个字符是'-'，直接跳出循环，之前传入的第一个参数是 -Xzygote,所以执行到这儿就跳出了，i=0
            break;
        }
        if (argv[i][1] == '-' && argv[i][2] == 0) {
            ++i; // Skip --.
            break;
        }

        runtime.addOption(strdup(argv[i]));
        ALOGV("app_process main add option '%s'", argv[i]);
    }

    // Parse runtime arguments.  Stop at first unrecognized option.
    bool zygote = false;
    bool startSystemServer = false;
    bool application = false;
    String8 niceName;
    String8 className;

    ++i;  // Skip unused "parent dir" argument.
    //跳过一个参数，之前跳过了-Xzygote，这里继续跳过 /system/bin ,也就是所谓的 "parent dir"
    while (i < argc) {
        const char* arg = argv[i++];
        if (strcmp(arg, "--zygote") == 0) {//表示是zygote启动模式
            zygote = true;
            niceName = ZYGOTE_NICE_NAME;//这个值根据平台可能是zygote64或zygote
        } else if (strcmp(arg, "--start-system-server") == 0) {//需要启动SystemServer
            startSystemServer = true;
        } else if (strcmp(arg, "--application") == 0) {//表示是application启动模式，也就是普通应用程序
            application = true;
        } else if (strncmp(arg, "--nice-name=", 12) == 0) {//进程别名
            niceName.setTo(arg + 12);
        } else if (strncmp(arg, "--", 2) != 0) {//application启动的class
            className.setTo(arg);
            break;
        } else {
            --i;
            break;
        }
    }

    Vector<String8> args;
    if (!className.isEmpty()) {//className不为空，说明是application启动模式
        // We're not in zygote mode, the only argument we need to pass
        // to RuntimeInit is the application argument.
        //
        // The Remainder of args get passed to startup class main(). Make
        // copies of them before we overwrite them with the process name.
        args.add(application ? String8("application") : String8("tool"));
        runtime.setClassNameAndArgs(className, argc - i, argv + i);//将className和参数设置给runtime

        if (!LOG_NDEBUG) {//打印class带的参数
          String8 restOfArgs;
          char* const* argv_new = argv + i;
          int argc_new = argc - i;
          for (int k = 0; k < argc_new; ++k) {
            restOfArgs.append("\"");
            restOfArgs.append(argv_new[k]);
            restOfArgs.append("\" ");
          }
          ALOGV("Class name = %s, args = %s", className.string(), restOfArgs.string());
        }
    } else { //zygote启动模式
        // We're in zygote mode.
        maybeCreateDalvikCache(); //新建Dalvik的缓存目录

        if (startSystemServer) {//加入start-system-server参数
            args.add(String8("start-system-server"));
        }

        char prop[PROP_VALUE_MAX];
        if (property_get(ABI_LIST_PROPERTY, prop, NULL) == 0) {
            LOG_ALWAYS_FATAL("app_process: Unable to determine ABI list from property %s.",
                ABI_LIST_PROPERTY);
            return 11;
        }

        String8 abiFlag("--abi-list=");
        abiFlag.append(prop);
        args.add(abiFlag); //加入--abi-list=参数

        // In zygote mode, pass all remaining arguments to the zygote
        // main() method.
        for (; i < argc; ++i) {//将剩下的参数加入args
            args.add(String8(argv[i]));
        }
    }

    if (!niceName.isEmpty()) {//设置进程别名
        runtime.setArgv0(niceName.string(), true /* setProcName */);
    }

    if (zygote) { //如果是zygote启动模式，则加载ZygoteInit
        runtime.start("com.android.internal.os.ZygoteInit", args, zygote);
    } else if (className) {//如果是application启动模式，则加载RuntimeInit
        runtime.start("com.android.internal.os.RuntimeInit", args, zygote);
    } else {
        fprintf(stderr, "Error: no class name or --zygote supplied.\n");
        app_usage();
        LOG_ALWAYS_FATAL("app_process: no class name or --zygote supplied.");
    }
}
```