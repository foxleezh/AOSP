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
platform/frameworks/base/cmds/app_process/app_main.cpp
platform/frameworks/base/core/jni/AndroidRuntime.cpp
platform/libnativehelper/JniInvocation.cpp
platform/frameworks/base/core/java/com/android/internal/os/ZygoteInit.java
platform/libcore/dalvik/src/main/java/dalvik/system/ZygoteHooks
platform/art/runtime/native/dalvik_system_ZygoteHooks.cc
platform/art/runtime/runtime.h
platform/art/runtime/runtime.cc
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

## 二、zygote参数解析
platform/frameworks/base/cmds/app_process/app_main.cpp

在app_main.cpp的main函数中，主要做的事情就是参数解析. 这个函数有两种启动模式：
- 一种是zygote模式，也就是初始化zygote进程，传递的参数有--start-system-server --socket-name=zygote，前者表示启动SystemServer，后者指定socket的名称
- 一种是application模式，也就是启动普通应用程序，传递的参数有class名字以及class带的参数

两者最终都是调用AppRuntime对象的start函数，加载ZygoteInit或RuntimeInit两个Java类，并将之前整理的参数传入进去

由于本篇讲的是zygote进程启动流程，因此接下来我只讲解ZygoteInit的加载.

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

## 三、创建虚拟机

这部分我将分两步讲解，一是虚拟机的创建，二是调用ZygoteInit类的main函数 

### 3.1 创建虚拟机、注册JNI函数
platform/frameworks/base/core/jni/AndroidRuntime.cpp

前半部分主要是初始化JNI，然后创建虚拟机，注册一些JNI函数，我将分开一个个单独讲

```C

void AndroidRuntime::start(const char* className, const Vector<String8>& options, bool zygote)
{

    ... //打印一些日志，获取ANDROID_ROOT环境变量

    /* start the virtual machine */
    JniInvocation jni_invocation;
    jni_invocation.Init(NULL);//初始化JNI,加载libart.so
    JNIEnv* env;
    if (startVm(&mJavaVM, &env, zygote) != 0) {//创建虚拟机
        return;
    }
    onVmCreated(env);//表示虚拟创建完成，但是里面是空实现

    /*
     * Register android functions.
     */
    if (startReg(env) < 0) {注册JNI函数
        ALOGE("Unable to register all android natives\n");
        return;
    }
    
    ... //JNI方式调用ZygoteInit类的main函数
}
```

#### 3.1.1 JniInvocation.Init
定义在platform/libnativehelper/JniInvocation.cpp

Init函数主要作用是初始化JNI，具体工作是首先通过dlopen加载libart.so获得其句柄，然后调用dlsym从libart.so中找到
JNI_GetDefaultJavaVMInitArgs、JNI_CreateJavaVM、JNI_GetCreatedJavaVMs三个函数地址，赋值给对应成员属性，
这三个函数会在后续虚拟机创建中调用.

```C
bool JniInvocation::Init(const char* library) {
#ifdef __ANDROID__
  char buffer[PROP_VALUE_MAX];
#else
  char* buffer = NULL;
#endif
  library = GetLibrary(library, buffer);//默认返回 libart.so
  // Load with RTLD_NODELETE in order to ensure that libart.so is not unmapped when it is closed.
  // This is due to the fact that it is possible that some threads might have yet to finish
  // exiting even after JNI_DeleteJavaVM returns, which can lead to segfaults if the library is
  // unloaded.
  const int kDlopenFlags = RTLD_NOW | RTLD_NODELETE;
  /*
   * 1.dlopen功能是以指定模式打开指定的动态链接库文件，并返回一个句柄
   * 2.RTLD_NOW表示需要在dlopen返回前，解析出所有未定义符号，如果解析不出来，在dlopen会返回NULL
   * 3.RTLD_NODELETE表示在dlclose()期间不卸载库，并且在以后使用dlopen()重新加载库时不初始化库中的静态变量
   */
  handle_ = dlopen(library, kDlopenFlags); // 获取libart.so的句柄
  if (handle_ == NULL) { //获取失败打印错误日志并尝试再次打开libart.so
    if (strcmp(library, kLibraryFallback) == 0) {
      // Nothing else to try.
      ALOGE("Failed to dlopen %s: %s", library, dlerror());
      return false;
    }
    // Note that this is enough to get something like the zygote
    // running, we can't property_set here to fix this for the future
    // because we are root and not the system user. See
    // RuntimeInit.commonInit for where we fix up the property to
    // avoid future fallbacks. http://b/11463182
    ALOGW("Falling back from %s to %s after dlopen error: %s",
          library, kLibraryFallback, dlerror());
    library = kLibraryFallback;
    handle_ = dlopen(library, kDlopenFlags);
    if (handle_ == NULL) {
      ALOGE("Failed to dlopen %s: %s", library, dlerror());
      return false;
    }
  }

  /*
   * 1.FindSymbol函数内部实际调用的是dlsym
   * 2.dlsym作用是根据 动态链接库 操作句柄(handle)与符号(symbol)，返回符号对应的地址
   * 3.这里实际就是从libart.so中将JNI_GetDefaultJavaVMInitArgs等对应的地址存入&JNI_GetDefaultJavaVMInitArgs_中
   */
  if (!FindSymbol(reinterpret_cast<void**>(&JNI_GetDefaultJavaVMInitArgs_),
                  "JNI_GetDefaultJavaVMInitArgs")) {
    return false;
  }
  if (!FindSymbol(reinterpret_cast<void**>(&JNI_CreateJavaVM_),
                  "JNI_CreateJavaVM")) {
    return false;
  }
  if (!FindSymbol(reinterpret_cast<void**>(&JNI_GetCreatedJavaVMs_),
                  "JNI_GetCreatedJavaVMs")) {
    return false;
  }
  return true;
}
```

#### 3.1.2 startVm
定义在platform/frameworks/base/core/jni/AndroidRuntime.cpp

这个函数特别长，但是里面做的事情很单一，其实就是从各种系统属性中读取一些参数，然后通过addOption设置到AndroidRuntime的mOptions数组中存起来，
另外就是调用之前从libart.so中找到JNI_CreateJavaVM函数，并将这些参数传入，由于本篇主要讲zygote启动流程，因此关于虚拟机的实现就不深入探究了
```C
int AndroidRuntime::startVm(JavaVM** pJavaVM, JNIEnv** pEnv, bool zygote)
{
    JavaVMInitArgs initArgs;
    ...
    addOption("exit", (void*) runtime_exit);各//将参数放入mOptions数组中
    ...
    initArgs.version = JNI_VERSION_1_4;
    initArgs.options = mOptions.editArray();//将mOptions赋值给initArgs
    initArgs.nOptions = mOptions.size();
    initArgs.ignoreUnrecognized = JNI_FALSE;
    if (JNI_CreateJavaVM(pJavaVM, pEnv, &initArgs) < 0) {//调用libart.so的JNI_CreateJavaVM函数
            ALOGE("JNI_CreateJavaVM failed\n");
            return -1;
    }
    return 0;
}

extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, JNIEnv** p_env, void* vm_args) {
  return JniInvocation::GetJniInvocation().JNI_CreateJavaVM(p_vm, p_env, vm_args);
}

jint JniInvocation::JNI_CreateJavaVM(JavaVM** p_vm, JNIEnv** p_env, void* vm_args) {
  return JNI_CreateJavaVM_(p_vm, p_env, vm_args);//调用之前初始化的JNI_CreateJavaVM_
}
```


#### 3.1.3 startReg
定义在platform/frameworks/base/core/jni/AndroidRuntime.cpp

startReg首先是设置了Android创建线程的处理函数，然后创建了一个200容量的局部引用作用域，用于确保不会出现OutOfMemoryExceptio，
最后就是调用register_jni_procs进行JNI注册

```C
int AndroidRuntime::startReg(JNIEnv* env)
{
    ATRACE_NAME("RegisterAndroidNatives");
    /*
     * This hook causes all future threads created in this process to be
     * attached to the JavaVM.  (This needs to go away in favor of JNI
     * Attach calls.)
     */
    androidSetCreateThreadFunc((android_create_thread_fn) javaCreateThreadEtc);
    //设置Android创建线程的函数javaCreateThreadEtc，这个函数内部是通过Linux的clone来创建线程的

    ALOGV("--- registering native functions ---\n");

    /*
     * Every "register" function calls one or more things that return
     * a local reference (e.g. FindClass).  Because we haven't really
     * started the VM yet, they're all getting stored in the base frame
     * and never released.  Use Push/Pop to manage the storage.
     */
    env->PushLocalFrame(200);//创建一个200容量的局部引用作用域,这个局部引用其实就是局部变量

    if (register_jni_procs(gRegJNI, NELEM(gRegJNI), env) < 0) { //注册JNI函数
        env->PopLocalFrame(NULL);
        return -1;
    }
    env->PopLocalFrame(NULL);//释放局部引用作用域

    //createJavaThread("fubar", quickTest, (void*) "hello");

    return 0;
}
```

#### 3.1.4 register_jni_procs
定义在platform/frameworks/base/core/jni/AndroidRuntime.cpp

它的处理是交给RegJNIRec的mProc,RegJNIRec是个很简单的结构体，mProc是个函数指针
```C
static int register_jni_procs(const RegJNIRec array[], size_t count, JNIEnv* env)
{
    for (size_t i = 0; i < count; i++) {
        if (array[i].mProc(env) < 0) { //调用mProc
#ifndef NDEBUG
            ALOGD("----------!!! %s failed to load\n", array[i].mName);
#endif
            return -1;
        }
    }
    return 0;
}

struct RegJNIRec {
   int (*mProc)(JNIEnv*);
};

```

我们看看register_jni_procs传入的RegJNIRec数组gRegJNI,里面就是一堆的函数指针
```C
static const RegJNIRec gRegJNI[] = {
    REG_JNI(register_com_android_internal_os_RuntimeInit),
    REG_JNI(register_com_android_internal_os_ZygoteInit),
    REG_JNI(register_android_os_SystemClock),
    REG_JNI(register_android_util_EventLog),
    REG_JNI(register_android_util_Log),
    REG_JNI(register_android_util_MemoryIntArray)
    ...
}
```

我们随便看一个register_com_android_internal_os_ZygoteInit,这实际上是自定义JNI函数并进行动态注册的标准写法,
内部是调用JNI的RegisterNatives,这样注册后，Java类ZygoteInit的native方法nativeZygoteInit就会调用com_android_internal_os_ZygoteInit_nativeZygoteInit函数
```C
int register_com_android_internal_os_ZygoteInit(JNIEnv* env)
{
    const JNINativeMethod methods[] = {
        { "nativeZygoteInit", "()V",
            (void*) com_android_internal_os_ZygoteInit_nativeZygoteInit },
    };
    return jniRegisterNativeMethods(env, "com/android/internal/os/ZygoteInit",
        methods, NELEM(methods));
}
```

以上便是第一部分的内容，主要工作是从libart.so提取出JNI初始函数JNI_CreateJavaVM，然后读取一些系统属性作为参数调用JNI_CreateJavaVM创建虚拟机，
在虚拟机创建完成后，动态注册一些native函数，接下来我们讲第二部分，反射调用ZygoteInit类的main函数

### 3.2 反射调用ZygoteInit类的main函数

虚拟机创建完成后，我们就可以用JNI反射调用Java了，其实接下来的语法用过JNI的都应该比较熟悉了，直接是CallStaticVoidMethod反射调用ZygoteInit的main函数


```C
void AndroidRuntime::start(const char* className, const Vector<String8>& options, bool zygote)
{
    /*
     * We want to call main() with a String array with arguments in it.
     * At present we have two arguments, the class name and an option string.
     * Create an array to hold them.
     */
     
    //接下来的这些语法大家应该比较熟悉了，都是JNI里的语法，主要作用就是调用ZygoteInit类的main函数 
    jclass stringClass;
    jobjectArray strArray;
    jstring classNameStr;

    stringClass = env->FindClass("java/lang/String");
    assert(stringClass != NULL);
    strArray = env->NewObjectArray(options.size() + 1, stringClass, NULL);
    assert(strArray != NULL);
    classNameStr = env->NewStringUTF(className);
    assert(classNameStr != NULL);
    env->SetObjectArrayElement(strArray, 0, classNameStr);

    for (size_t i = 0; i < options.size(); ++i) {
        jstring optionsStr = env->NewStringUTF(options.itemAt(i).string());
        assert(optionsStr != NULL);
        env->SetObjectArrayElement(strArray, i + 1, optionsStr);
    }

    /*
     * Start VM.  This thread becomes the main thread of the VM, and will
     * not return until the VM exits.
     */
    char* slashClassName = toSlashClassName(className);//将字符中的.转换为/
    jclass startClass = env->FindClass(slashClassName);//找到class
    if (startClass == NULL) {
        ALOGE("JavaVM unable to locate class '%s'\n", slashClassName);
        /* keep going */
    } else {
        jmethodID startMeth = env->GetStaticMethodID(startClass, "main",
            "([Ljava/lang/String;)V");
        if (startMeth == NULL) {
            ALOGE("JavaVM unable to find main() in '%s'\n", className);
            /* keep going */
        } else {
            env->CallStaticVoidMethod(startClass, startMeth, strArray);//调用main函数

#if 0
            if (env->ExceptionCheck())
                threadExitUncaughtException(env);
#endif
        }
    }
    free(slashClassName);

    ALOGD("Shutting down VM\n");
    if (mJavaVM->DetachCurrentThread() != JNI_OK)//退出当前线程
        ALOGW("Warning: unable to detach main thread\n");
    if (mJavaVM->DestroyJavaVM() != 0) //创建一个线程，该线程会等待所有子线程结束后关闭虚拟机
        ALOGW("Warning: VM did not shut down cleanly\n");
}
```

## 四、进入Java世界

虚拟机创建好之后，系统就可以运行Java代码了，终于是我们熟悉的语法，读起来相对简单些，但是Java代码经常要用JNI调用native代码，
如果我们要了解native的具体实现，我们就必须找到对应的JNI注册函数，说实话还挺不好找的，有些我都是全局搜索才找到的，我具体讲代码的时候再说如何去找native代码实现。
接下来我将分析ZygoteInit的main函数，我将拆分为四个部分来讲：
- 开启性能统计
- 注册Socket
- 加载资源
- 开启守护循环

### 4.1 开启性能统计
定义在platform/frameworks/base/core/java/com/android/internal/os/ZygoteInit.java

main函数最开始new了一个ZygoteServer，这个后续会用到，然后设置标记，不允许新建线程，为什么不允许多线程呢？
这主要是担心用户创建app时，多线程情况下某些预先加载的资源没加载好，这时去调用会出问题. 接着设置了zygote进程的进程组id，
最后便是一系列性能统计相关的动作


```java
public static void main(String argv[]) {
        ZygoteServer zygoteServer = new ZygoteServer();

        // Mark zygote start. This ensures that thread creation will throw
        // an error.
        ZygoteHooks.startZygoteNoThreadCreation(); //设置标记，不允许新建线程

        // Zygote goes into its own process group.
        try {
            Os.setpgid(0, 0); //设置zygote进程组id为zygote的pid
        } catch (ErrnoException ex) {
            throw new RuntimeException("Failed to setpgid(0,0)", ex);
        }

        try {
            // Report Zygote start time to tron unless it is a runtime restart
            if (!"1".equals(SystemProperties.get("sys.boot_completed"))) {
                MetricsLogger.histogram(null, "boot_zygote_init",
                        (int) SystemClock.elapsedRealtime());//记录boot_zygote_init时间戳
            }

            String bootTimeTag = Process.is64Bit() ? "Zygote64Timing" : "Zygote32Timing";
            BootTimingsTraceLog bootTimingsTraceLog = new BootTimingsTraceLog(bootTimeTag,
                    Trace.TRACE_TAG_DALVIK);
            bootTimingsTraceLog.traceBegin("ZygoteInit"); //跟踪调试ZygoteInit
            RuntimeInit.enableDdms(); //开启DDMS
            // Start profiling the zygote initialization.
            SamplingProfilerIntegration.start(); //开始性能统计
            ...
            SamplingProfilerIntegration.writeZygoteSnapshot();//结束性能统计并写入文件

}

```

#### 4.1.1 startZygoteNoThreadCreation
定义在platform/libcore/dalvik/src/main/java/dalvik/system/ZygoteHooks中

这是一个native方法，其实这个方法作用并不复杂，只是设置一个boolean值而已，我特意在这儿讲是想告诉大家如何去追踪native方法的实现.
我们知道native方法有两种注册方式，一种是静态注册，一种动态注册。所谓静态注册就是根据函数名称和一些关键字就可以注册，比如startZygoteNoThreadCreation
要静态注册的话，它对应的实现函数应该是
```c
JNIEXPORT void JNICALL Java_dalvik_system_ZygoteHooks_startZygoteNoThreadCreation(JNIEnv *, jobject){
}

```
也就是说首先得有JNIEXPORT，JNICALL这些关键字，其实函数名称必须以Java开头，后面接的是native函数所在类的完整路径加native函数名，
最后参数及返回值要相同，参数会多出两个，一个是JNIEnv，表示JNI上下文，一个是jobject，表示调用native函数的对象. 只要你按照这个规则写，
Java的native函数就会自动调用这个C++层的函数。这种静态的注册方式有个不好的地方就是函数名太长，书写不方便，而且在首次调用时会有一个注册过程，
影响效率，那有没有其他方式呢？答案就是动态注册

其实大多数frameworks层的native函数都是用动态方式注册的，startZygoteNoThreadCreation函数也是，我们就以startZygoteNoThreadCreation为例.

```java

    /*
     * Called by the zygote when starting up. It marks the point when any thread
     * start should be an error, as only internal daemon threads are allowed there.
     */
    public static native void startZygoteNoThreadCreation(); 
```

我们怎么寻找startZygoteNoThreadCreation的实现呢？这里有个规律，Google工程师喜欢以native所在类的完整路径为C++的实现类名，比如
startZygoteNoThreadCreation所在类的完整路径是dalvik.system.ZygoteHooks，我们尝试寻找dalvik_system_ZygoteHooks这个文件，
果然出现了dalvik_system_ZygoteHooks.h和dalvik_system_ZygoteHooks.cc，我们看下dalvik_system_ZygoteHooks.cc
```C
static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(ZygoteHooks, nativePreFork, "()J"),
  NATIVE_METHOD(ZygoteHooks, nativePostForkChild, "(JIZLjava/lang/String;)V"),
  NATIVE_METHOD(ZygoteHooks, startZygoteNoThreadCreation, "()V"),
  NATIVE_METHOD(ZygoteHooks, stopZygoteNoThreadCreation, "()V"),
};

void register_dalvik_system_ZygoteHooks(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/ZygoteHooks");
}
```

```java
public static void main(String argv[]) {

            ...

            boolean startSystemServer = false;
            String socketName = "zygote";
            String abiList = null;
            boolean enableLazyPreload = false;
            for (int i = 1; i < argv.length; i++) {
                if ("start-system-server".equals(argv[i])) {
                    startSystemServer = true;
                } else if ("--enable-lazy-preload".equals(argv[i])) {
                    enableLazyPreload = true;
                } else if (argv[i].startsWith(ABI_LIST_ARG)) {
                    abiList = argv[i].substring(ABI_LIST_ARG.length());
                } else if (argv[i].startsWith(SOCKET_NAME_ARG)) {
                    socketName = argv[i].substring(SOCKET_NAME_ARG.length());
                } else {
                    throw new RuntimeException("Unknown command line argument: " + argv[i]);
                }
            }

            if (abiList == null) {
                throw new RuntimeException("No ABI list supplied.");
            }
            zygoteServer.registerServerSocket(socketName);
            ...
}
```

```java
 public static void main(String argv[]) {

            ...

            // In some configurations, we avoid preloading resources and classes eagerly.
            // In such cases, we will preload things prior to our first fork.
            if (!enableLazyPreload) {
                bootTimingsTraceLog.traceBegin("ZygotePreload");
                EventLog.writeEvent(LOG_BOOT_PROGRESS_PRELOAD_START,
                    SystemClock.uptimeMillis());
                preload(bootTimingsTraceLog);
                EventLog.writeEvent(LOG_BOOT_PROGRESS_PRELOAD_END,
                    SystemClock.uptimeMillis());
                bootTimingsTraceLog.traceEnd(); // ZygotePreload
            } else {
                Zygote.resetNicePriority();
            }

            // Finish profiling the zygote initialization.
            SamplingProfilerIntegration.writeZygoteSnapshot();

            // Do an initial gc to clean up after startup
            bootTimingsTraceLog.traceBegin("PostZygoteInitGC");
            gcAndFinalize();
            bootTimingsTraceLog.traceEnd(); // PostZygoteInitGC

            bootTimingsTraceLog.traceEnd(); // ZygoteInit
            // Disable tracing so that forked processes do not inherit stale tracing tags from
            // Zygote.
            Trace.setTracingEnabled(false);

            ...

}
```


```java
 public static void main(String argv[]) {

            ...

            // Zygote process unmounts root storage spaces.
            Zygote.nativeUnmountStorageOnInit();

            // Set seccomp policy
            Seccomp.setPolicy();

            ZygoteHooks.stopZygoteNoThreadCreation();

            if (startSystemServer) {
                startSystemServer(abiList, socketName, zygoteServer);
            }

            Log.i(TAG, "Accepting command socket connections");
            zygoteServer.runSelectLoop(abiList);

            zygoteServer.closeServerSocket();
        } catch (Zygote.MethodAndArgsCaller caller) {
            caller.run();
        } catch (Throwable ex) {
            Log.e(TAG, "System zygote died with exception", ex);
            zygoteServer.closeServerSocket();
            throw ex;
        }
    }
```
