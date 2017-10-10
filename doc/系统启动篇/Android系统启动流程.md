此文主要介绍Android启动后，从Init进程到Home界面的过程，首先上时序图:

![Android系统启动流程.png](http://upload-images.jianshu.io/upload_images/3387045-947b11181ad902e8.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

我将从时序图上的序号开始一一分解，图片不是很清晰，不过我会在序号上列出

先讲init进程到zygote进程，序号为1到8，这个过程主要是解析init.rc文件，然后将解析出的进程一一启动，其中最重要的进程就是zygote进程

####step 1.main
Android启动内核后，fork出的第一个进程即init进程，该进程的入口是init.cpp([/system/core/init/init.cpp](http://androidxref.com/6.0.1_r10/xref/system/core/init/init.cpp))的main方法
```C++
int main(int argc, char** argv) {
    ...

    init_parse_config_file("/init.rc");

   ...

    while (true) {
        if (!waiting_for_exec) {
            execute_one_command();
            restart_processes();
        }

       ...
    return 0;
}

```
init进程首先是去解析init.rc文件，这并不是普通的配置文件，而是由一种被称为“初始化语言”（Android Init Language，这里简称为AIL）的脚本写成的文件。
####step 2.init_parse_config_file

该方法调用的是init_parse.cpp([/system/core/init/init_parser.cpp](http://androidxref.com/6.0.1_r10/xref/system/core/init/init_parser.cpp))的init_parse_config_file方法

```
int init_parse_config_file(const char* path) {
    ...
    parse_config(path, data);
    ...
    return 0;
}
```
```
static void parse_config(const char *fn, const std::string& data)
{
    ...

    for (;;) {
        switch (next_token(&state)) {
        case T_EOF:
            state.parse_line(&state, 0, 0);
            goto parser_done;
        case T_NEWLINE:
            state.line++;
            if (nargs) {
                int kw = lookup_keyword(args[0]);
                if (kw_is(kw, SECTION)) {
                    state.parse_line(&state, 0, 0);
                    parse_new_section(&state, kw, nargs, args);
                } else {
                    state.parse_line(&state, nargs, args);
                }
                nargs = 0;
            }
            break;
        case T_TEXT:
            if (nargs < INIT_PARSER_MAXARGS) {
                args[nargs++] = state.text;
            }
            break;
        }
    }

parser_done:
    list_for_each(node, &import_list) {
         struct import *import = node_to_item(node, struct import, list);
         int ret;
         ret = init_parse_config_file(import->filename);
    ...
    }
}

```
关于init.rc的解析，本文不作深入讲解，可参考[[/system/core/init/readme.txt对init.rc的解释](http://blog.csdn.net/a345017062/article/details/6239204)](http://blog.csdn.net/a345017062/article/details/6239204)，总之这一步最重要的就是得到一个需要启动的进程列表

####step 3.restart_processes
回到init.cpp的main方法，开启一个无限循环去调用restart_processes方法,restart_processes方法一看名字就知道是去启动进程，启动哪些进程呢？也就是第2步中解析init.rc得到的进程列表！
```
static void restart_processes()
{
    process_needs_restart = 0;
    service_for_each_flags(SVC_RESTARTING,
                           restart_service_if_needed);
}
```
再来看看restart_processes具体内容，调用了service_for_each_flags方法，方法中传递了一个restart_service_if_needed参数，这不是个普通的参数，而是一个方法参数，熟悉C++的朋友可能一下就知道是怎么回事，但是对于我这种java开发为主的看到真是一脸蒙逼！简单说就是把方法的引用传递过去了，然后可以在方法体中直接调用。

####step 4.service_for_each_flags
这里跳转的是init_parser.cpp中的service_for_each_flags
```
void service_for_each_flags(unsigned matchflags,
                            void (*func)(struct service *svc))
{
    struct listnode *node;
    struct service *svc;
    list_for_each(node, &service_list) {
        svc = node_to_item(node, struct service, slist);
        if (svc->flags & matchflags) {
            func(svc);
        }
    }
}
```
list_for_each就是遍历列表，遍历的是service_list，之前解析init.rc的时候就是把解析到的service进程放在这个列表里了。在循环体里回调func方法，这个方法就是step3中通过方法参数传递过来的restart_service_if_needed方法

#### step 5.restart_service_if_needed
继续回到init.cpp中的restart_service_if_needed方法
```
static void restart_service_if_needed(struct service *svc)
{
        ...
        service_start(svc, NULL);
        ...
}
```
直接调用service_start方法

####step 6.service_start
```
void service_start(struct service *svc, const char *dynamic_args)
{
    ...
    pid_t pid = fork();
    ...
    execve(svc->args[0], (char**) arg_ptrs, (char**) ENV);
    ...  
}

```
该方法主要目的有两个，一是fork出新的进程，二是通过execve运行fork出来的进程，args[0]是进程名称

####step 7.fork
fork函数会复制一个新的进程返回pid

####step 8.execve(app_process)
在父进程中fork出一个子进程后，在子进程中f需要调用exec函数启动新的程序。exec函数一共有六个，其中execve为内核级系统调用，其他（execl，execle，execlp，execv，execvp）都是调用execve的库函数。该函数会调用对应进程的main方法


##小结
步骤1-8主要是解析init.rc文件，从文件中读出需要启动的进程，然后一一启动，我们可以抓开机trace看具体有哪些进程被依次fork出来：

```
Line 4974: [    3.123846] <0>.(2)[155:init]init: >>start execve(/sbin/ueventd): /sbin/ueventd
	Line 5460: [    6.995649] <1>.(0)[200:init]init: >>start execve(/system/bin/debuggerd): /system/bin/debuggerd
	Line 5464: [    7.002603] <0>.(0)[201:init]init: >>start execve(/system/bin/vold): /system/bin/vold
	Line 5516: [    8.123684] <0>.(0)[209:init]init: >>start execve(/system/bin/logd): /system/bin/logd
	Line 5584: [    8.246659] <1>.(1)[221:init]init: >>start execve(/system/bin/servicemanager): /system/bin/servicemanager
	Line 5590: [    8.276387] <0>.(1)[222:init]init: >>start execve(/system/bin/surfaceflinger): /system/bin/surfaceflinger
	Line 5676: [    8.328844] <3>.(1)[228:init]init: >>start execve(/vendor/bin/nvram_daemon): /vendor/bin/nvram_daemon
	Line 5735: [    8.357851] <3>.(1)[246:init]init: >>start execve(/vendor/bin/batterywarning): /vendor/bin/batterywarning
	Line 5769: [    8.376744] <3>.(1)[252:init]init: >>start execve(/system/bin/cameraserver): /system/bin/cameraserver
	Line 5779: [    8.384829] <3>.(0)[255:init]init: >>start execve(/system/bin/keystore): /system/bin/keystore
	Line 5803: [    8.400396] <3>.(1)[258:init]init: >>start execve(/system/bin/mediaserver): /system/bin/mediaserver
	Line 5805: [    8.401733] <3>.(2)[251:init]init: >>start execve(/system/bin/audioserver): /system/bin/audioserver
	Line 5815: [    8.417515] <3>.(2)[250:init]init: >>start execve(/system/bin/app_process): /system/bin/app_process
	Line 5817: [    8.423806] <2>.(2)[224:init]init: >>start execve(/system/bin/sh): /system/bin/sh
	Line 5823: [    8.436931] <3>.(3)[256:init]init: >>start execve(/system/bin/mediadrmserver): /system/bin/mediadrmserver
	Line 6329: [   10.084763] <1>.(3)[384:init]init: >>start execve(/sbin/adbd): /sbin/adbd
	Line 7039: [   11.081987] <3>.(0)[442:init]init: >>start execve(/system/bin/bootanimation): /system/bin/bootanimation
```
上面看到的第一个被init启动的进程是ueventd, 而 app_process 就是后面的zygote 进程, 我们下面将讲解zygote进程的启动流程

zygote进程意为孵化，可以理解为我们的app就是一个个小鸡，这些小鸡都是从zyogte进程孵化出来的，从这个进程开始，我们将从C++的世界切换到java的世界

####step 9.main
app_process进程的入口是([/frameworks/base/cmds/app_process/app_main.cpp](http://androidxref.com/6.0.1_r10/xref/frameworks/base/cmds/app_process/app_main.cpp))的main方法

```
int main(int argc, char* const argv[])
{
    ...
    AppRuntime runtime(argv[0], computeArgBlockSize(argc, argv));
    ...
    if (zygote) {
        runtime.start("com.android.internal.os.ZygoteInit", args, zygote);
    } else if (className) {
        runtime.start("com.android.internal.os.RuntimeInit", args, zygote);
    } else {
       ...
    }
}
```
这里先new了一个实例AppRuntime ，什么？哪里new了，我没看到new啊，这又是java人看不懂的，C++中有一种实例化方式是这样的A a(args,args);这个相当于A a=new A(args,args)。接着调用runtime.start

####step 10.runtime=new AndroidRuntime

前一步的AppRuntime其实是AndroidRuntime的子类

####step 11.runtime.start
这步调用的是AndroidRuntime.cpp([frameworks/base/core/jni/AndroidRuntime.cpp](http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/jni/AndroidRuntime.cpp))的start方法
```
void AndroidRuntime::start(const char* className, const Vector<String8>& options, bool zygote)
{
    ...
    JniInvocation jni_invocation;
    jni_invocation.Init(NULL);
    JNIEnv* env;
    if (startVm(&mJavaVM, &env, zygote) != 0) {
        return;
    }
    onVmCreated(env);

    /*
     * Register android functions.
     */
    if (startReg(env) < 0) {
        ALOGE("Unable to register all android natives\n");
        return;
    }

    ...
    jclass startClass = env->FindClass(slashClassName);
    ...
    jmethodID startMeth = env->GetStaticMethodID(startClass, "main",
            "([Ljava/lang/String;)V");
    ...
    env->CallStaticVoidMethod(startClass, startMeth, strArray);
    ...
}
```
start方法先启动了虚拟机，正式从C++的环境切换到了java虚拟机的环境，我们看到了熟悉的JNI方法
####step 12.startVm
这步是启动虚拟机，虚拟机内部实现比较复杂，本文就不展开了
####step 13.CallStaticVoidMethod(ZygoteInit,main)
这些方法比较熟悉，Findclass("ZygoteInit")找到ZygoteInit.java类，GetStaticMethodID(startClass, "main", "([Ljava/lang/String;)V");得到main方法，CallStaticVoidMethod(startClass, startMeth, strArray);调用main方法

# 小结
步骤9-13主要是启动了Zygote进程，然后在进程中实例化了Java的虚拟机，进入到Java运行环境


####step 14.main
通过JNI层的CallStaticVoidMethod方法，调用了ZygoteInit.java([frameworks/base/core/java/com/android/internal/os/ZygoteInit.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/java/com/android/internal/os/ZygoteInit.java))的main方法
```
public static void main(String argv[]) {
          ...
          try {
            registerZygoteSocket(socketName);
          ...
            preload();
          ...
            if (startSystemServer) {
                startSystemServer(abiList, socketName);
          ...
          } catch (MethodAndArgsCaller caller) {
            caller.run();
          }
          ...
    }
```
首先注册socket监听，socket名字为“zygote”，用于接受子进程创建req，然后调用preload方法做一些初始化的操作，然后启动SystemServer，注意这里有个try catch语句，之后代码会主动抛出MethodAndArgsCaller异常，然后调用caller.run();
####step 15.registerZygoteSocket
```
private static void registerZygoteSocket(String socketName) {
        if (sServerSocket == null) {
           ...
            try {
                FileDescriptor fd = new FileDescriptor();
                fd.setInt$(fileDesc);
                sServerSocket = new LocalServerSocket(fd);
            } 
          ...
        }
    }
```
new一个LocalServerSocket的实例并返回给sServerSocket，用于子进程与zygote进程之间的通信

####16.preload
```
static void preload() {
        Log.d(TAG, "begin preload");
        Trace.traceBegin(Trace.TRACE_TAG_DALVIK, "BeginIcuCachePinning");
        beginIcuCachePinning();
        Trace.traceEnd(Trace.TRACE_TAG_DALVIK);
        Trace.traceBegin(Trace.TRACE_TAG_DALVIK, "PreloadClasses");
        preloadClasses();
        Trace.traceEnd(Trace.TRACE_TAG_DALVIK);
        Trace.traceBegin(Trace.TRACE_TAG_DALVIK, "PreloadResources");
        preloadResources();
        Trace.traceEnd(Trace.TRACE_TAG_DALVIK);
        Trace.traceBegin(Trace.TRACE_TAG_DALVIK, "PreloadOpenGL");
        preloadOpenGL();
        Trace.traceEnd(Trace.TRACE_TAG_DALVIK);
        preloadSharedLibraries();
        preloadTextResources();
        // Ask the WebViewFactory to do any initialization that must run in the zygote process,
        // for memory sharing purposes.
        WebViewFactory.prepareWebViewInZygote();
        endIcuCachePinning();
        warmUpJcaProviders();
        Log.d(TAG, "end preload");
    }
```
加载一些系统资源，OpenGL等
####step 17.startSystemServer
```
private static boolean startSystemServer(String abiList, String socketName)
            throws MethodAndArgsCaller, RuntimeException {
        ...
        String args[] = {
            "--setuid=1000",
            "--setgid=1000",
            "--setgroups=1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1018,1021,1032,3001,3002,3003,3006,3007,3009,3010",
            "--capabilities=" + capabilities + "," + capabilities,
            "--nice-name=system_server",
            "--runtime-args",
            "com.android.server.SystemServer",
        };
        ...

        handleSystemServerProcess(parsedArgs);
       
    }
```
直接交给handleSystemServerProcess处理
####step 18.handleSystemServerProcess
```
    private static void handleSystemServerProcess(
            ZygoteConnection.Arguments parsedArgs)
            throws ZygoteInit.MethodAndArgsCaller {

       ...
        //之前传进来的invokeWith为null，走eles逻辑
        if (parsedArgs.invokeWith != null) {
            String[] args = parsedArgs.remainingArgs;
            if (systemServerClasspath != null) {
                String[] amendedArgs = new String[args.length + 2];
                amendedArgs[0] = "-cp";
                amendedArgs[1] = systemServerClasspath;
                System.arraycopy(parsedArgs.remainingArgs, 0, amendedArgs, 2, parsedArgs.remainingArgs.length);
            }

            WrapperInit.execApplication(parsedArgs.invokeWith,
                    parsedArgs.niceName, parsedArgs.targetSdkVersion,
                    VMRuntime.getCurrentInstructionSet(), null, args);
        } else {
            ClassLoader cl = null;
            if (systemServerClasspath != null) {
                cl = new PathClassLoader(systemServerClasspath, ClassLoader.getSystemClassLoader());
                Thread.currentThread().setContextClassLoader(cl);
            }
            RuntimeInit.zygoteInit(parsedArgs.targetSdkVersion, parsedArgs.remainingArgs, cl);
        }
    }
```
走else逻辑，new一个PathClassLoader，然后进入RuntimeInit.zygoteInit方法

####step 19.zygoteInit
这里调用RuntimeInit([frameworks/base/core/java/com/android/internal/os/RuntimeInit.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/java/com/android/internal/os/RuntimeInit.java))的zygoteInit方法
```
public static final void zygoteInit(int targetSdkVersion, String[] argv, ClassLoader classLoader)
            throws ZygoteInit.MethodAndArgsCaller {
        ...

        commonInit();
        nativeZygoteInit();
        applicationInit(targetSdkVersion, argv, classLoader);
    }
```
commonInit方法用于公共部分初始化：handler、timezone、user agent等，nativeZygoteInit用于调用native函数启动binder线程池用于支持binder通信，继续看applicationInit方法

####step 20.applicationInit
```
 private static void applicationInit(int targetSdkVersion, String[] argv, ClassLoader classLoader)
            throws ZygoteInit.MethodAndArgsCaller {
       ...
        invokeStaticMain(args.startClass, args.startArgs, classLoader);
    }
```
这个方法会抛出ZygoteInit.MethodAndArgsCalle异常，这个之前step14讲过会捕获这个异常，继续看invokeStaticMain方法

####step 21.invokeStaticMain
```
private static void invokeStaticMain(String className, String[] argv, ClassLoader classLoader)
            throws ZygoteInit.MethodAndArgsCaller {
        Class<?> cl;

        try {
            cl = Class.forName(className, true, classLoader);
        } catch (ClassNotFoundException ex) {
            throw new RuntimeException(
                    "Missing class when invoking static main " + className,
                    ex);
        }

        Method m;
        try {
            m = cl.getMethod("main", new Class[] { String[].class });
        } catch (NoSuchMethodException ex) {
            throw new RuntimeException(
                    "Missing static main on " + className, ex);
        } catch (SecurityException ex) {
            throw new RuntimeException(
                    "Problem getting static main on " + className, ex);
        }

        int modifiers = m.getModifiers();
        if (! (Modifier.isStatic(modifiers) && Modifier.isPublic(modifiers))) {
            throw new RuntimeException(
                    "Main method is not public and static on " + className);
        }

        /*
         * This throw gets caught in ZygoteInit.main(), which responds
         * by invoking the exception's run() method. This arrangement
         * clears up all the stack frames that were required in setting
         * up the process.
         */
        throw new ZygoteInit.MethodAndArgsCaller(m, argv);
    }
```
####step 22.throw new ZygoteInit.MethodAndArgsCalle
这里cl为com.android.server.SystemServe类，m为main方，最后主动抛出ZygoteInit.MethodAndArgsCaller异常，我们来看看这个MethodAndArgsCaller类
```
public static class MethodAndArgsCaller extends Exception
            implements Runnable {
        /** method to call */
        private final Method mMethod;

        /** argument array */
        private final String[] mArgs;

        public MethodAndArgsCaller(Method method, String[] args) {
            mMethod = method;
            mArgs = args;
        }

        public void run() {
            try {
                mMethod.invoke(null, new Object[] { mArgs });
            } catch (IllegalAccessException ex) {
                throw new RuntimeException(ex);
            } catch (InvocationTargetException ex) {
                Throwable cause = ex.getCause();
                if (cause instanceof RuntimeException) {
                    throw (RuntimeException) cause;
                } else if (cause instanceof Error) {
                    throw (Error) cause;
                }
                throw new RuntimeException(ex);
            }
        }
    }
```
这个类很简单，就是封装的一个run方法，直接反射调用mMethod方法

####step 23.caller.run
```
    try{
      ...
    }catch (MethodAndArgsCaller caller) {
      caller.run();
    } 
```
ZygoteInit的main方法捕获异常后直接调用run方法，也就是执行com.android.server.SystemServe的main方法

# 小结
步骤14-23主要是进行Zygote进程的一些初始化操作，加入Socket监听，加载一些必要的系统资源，然后就是启动SystemServer，接下来就开始进入系统关键服务的启动流程了，这一过程会有一大堆的系统服务被启动
####step 24.main
上一步调用SystemServer([/frameworks/base/services/java/com/android/server/SystemServer.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/services/java/com/android/server/SystemServer.java))的main方法
```
    public static void main(String[] args) {
        new SystemServer().run();
    }
```
直接调用run方法
```
private void run() {
       ...
        Looper.prepareMainLooper();
       ....
        createSystemContext();
        ....
        mSystemServiceManager = new SystemServiceManager(mSystemContext);
        ...
        try {
            startBootstrapServices();
            startCoreServices();
            startOtherServices();
        } catch (Throwable ex) {
           ...
        }
        Looper.loop();
    }

```
首先初始化Looper，android基本所有的进程都有一个looper，用来处理消息，接着创建Context
```
 private void createSystemContext() {
        ActivityThread activityThread = ActivityThread.systemMain();
        mSystemContext = activityThread.getSystemContext();
        mSystemContext.setTheme(android.R.style.Theme_DeviceDefault_Light_DarkActionBar);
    }
```
先调用ActivityThread([/frameworks/base/core/java/android/app/ActivityThread.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/java/android/app/ActivityThread.java))的systemMain方法
```
public static ActivityThread systemMain() {
        ...
        ActivityThread thread = new ActivityThread();
        thread.attach(true);
        return thread;
    }
```
systemtMain方法用于初始化ActivityThread，也就是所谓的UI线程，并调用attach方法
```
private void attach(boolean system) {
           ...
         
            try {
                mInstrumentation = new Instrumentation();
                ...
            } catch (Exception e) {
                throw new RuntimeException(
                        "Unable to instantiate Application():" + e.toString(), e);
            }
        }
      ....
    }
```
这里创建了mInstrumentation，这个是UI线程的事务执行者，基本所以重要的事都是它一手操办，回到createSystemContext方法，接着调用activityThread.getSystemContext();
```
public ContextImpl getSystemContext() {
        synchronized (this) {
            if (mSystemContext == null) {
                mSystemContext = ContextImpl.createSystemContext(this);
            }
            return mSystemContext;
        }
    }
```
也就是说我们android系统中经常用到的Context就是ContextImpl，这个类的重要性也不必多说，基本上我们想要的资源都在它这儿拿，继续回来run方法调用new SystemServiceManager(mSystemContext);
####step 25.new SystemServiceManager
SystemServiceManager([/frameworks/base/services/core/java/com/android/server/SystemServiceManager.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/services/core/java/com/android/server/SystemServiceManager.java))
这个类如其名，就是系统服务管家，它主要负责创建和管理各种系统服务，接下来会讲到
####step 26.startBootstrapServices
```
private void startBootstrapServices() {
        Installer installer = mSystemServiceManager.startService(Installer.class);

        mActivityManagerService = mSystemServiceManager.startService(
                ActivityManagerService.Lifecycle.class).getService();
        ...
        mPowerManagerService = mSystemServiceManager.startService(PowerManagerService.class);
        ...
        mSystemServiceManager.startService(LightsService.class);

        // Display manager is needed to provide display metrics before package manager
        // starts up.
        mDisplayManagerService = mSystemServiceManager.startService(DisplayManagerService.class);
        ...
        mActivityManagerService.setSystemProcess();
        ...
    }
```
这里启动了好几个服务，都是由上一步的SystemServiceManager来启动的，我们来看看startService方法

####step 27.startService(ActivityManagerService)
```
private final ArrayList<SystemService> mServices = new ArrayList<SystemService>();
public <T extends SystemService> T startService(Class<T> serviceClass) {
        ...
        final T service;
        try {
            Constructor<T> constructor = serviceClass.getConstructor(Context.class);
            service = constructor.newInstance(mContext);
        } catch (InstantiationException ex) {
           ... 
        }
        mServices.add(service);

        // Start it.
        try {
            service.onStart();
        } catch (RuntimeException ex) {
         ...
        }
        return service;
    }
```
主要工作是实例化一个服务类，然后加入Arraylist列表，然后调用服务的onStart方法，这些启动的服务中比较重要的是ActivityManagerService，这个服务管理着所有的Activity，我们开发中经常调用的startActivity方法最终都是由它来处理的
####step 28.constructor.newInstance
这些服务是通过反射来实例化的，我们回到startBootstrapServices方法，最后调用了ActivityManagerService的setSystemProcess方法
####step 29.setSystemProcess

ActivityManagerService([/frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java))
```
 public void setSystemProcess() {
        try {
            ServiceManager.addService(Context.ACTIVITY_SERVICE, this, true);
            ServiceManager.addService(ProcessStats.SERVICE_NAME, mProcessStats);
            ServiceManager.addService("meminfo", new MemBinder(this));
            ServiceManager.addService("gfxinfo", new GraphicsBinder(this));
            ServiceManager.addService("dbinfo", new DbBinder(this));
            if (MONITOR_CPU_USAGE) {
                ServiceManager.addService("cpuinfo", new CpuBinder(this));
            }
            ServiceManager.addService("permission", new PermissionController(this));
            ServiceManager.addService("processinfo", new ProcessInfoService(this));

            ...
    }
```
这里调用了ServiceManager的addService方法
####step 30.addService
```
public static void addService(String name, IBinder service, boolean allowIsolated) {
        try {
            getIServiceManager().addService(name, service, allowIsolated);
        } catch (RemoteException e) {
            Log.e(TAG, "error in addService", e);
        }
    }
```
```
 private static IServiceManager getIServiceManager() {
        if (sServiceManager != null) {
            return sServiceManager;
        }

        // Find the service manager
        sServiceManager = ServiceManagerNative.asInterface(BinderInternal.getContextObject());
        return sServiceManager;
    }
```
```
static public IServiceManager asInterface(IBinder obj)
    {
        if (obj == null) {
            return null;
        }
        IServiceManager in =
            (IServiceManager)obj.queryLocalInterface(descriptor);
        if (in != null) {
            return in;
        }
        
        return new ServiceManagerProxy(obj);
    }
```
这里通过代理调用了ServiceManagerProxy的addService方法，
```
public void addService(String name, IBinder service, boolean allowIsolated)
            throws RemoteException {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        data.writeInterfaceToken(IServiceManager.descriptor);
        data.writeString(name);
        data.writeStrongBinder(service);
        data.writeInt(allowIsolated ? 1 : 0);
        mRemote.transact(ADD_SERVICE_TRANSACTION, data, reply, 0);
        reply.recycle();
        data.recycle();
    }
```
熟悉AIDL的同学应该一下就看清楚这就是AIDL的标准写法，这里面涉及到Binder的进程间通信，transact方法会通过层层调用，最终在系统的内核注册对应名字的服务，以便于今后能过getService来获取服务，我们回到最初的run方法，接着startBootstrapServices方法调用了startCoreServices方法和startOtherServices方法
####step 31.startCoreServices
```
 private void startCoreServices() {
        // Tracks the battery level.  Requires LightService.
        mSystemServiceManager.startService(BatteryService.class);

        // Tracks application usage stats.
        mSystemServiceManager.startService(UsageStatsService.class);
        mActivityManagerService.setUsageStatsManager(
                LocalServices.getService(UsageStatsManagerInternal.class));
        // Update after UsageStatsService is available, needed before performBootDexOpt.
        mPackageManagerService.getUsageStatsIfNoPackageUsageInfo();

        // Tracks whether the updatable WebView is in a ready state and watches for update installs.
        mSystemServiceManager.startService(WebViewUpdateService.class);
    }
```
这里启动了3个服务BatteryService、UsageStatsService、WebViewUpdateService
####step 32.startOtherServices
```
private void startOtherServices() {
        ...
       ServiceManager.addService(Context.WINDOW_SERVICE, wm);
       ServiceManager.addService(Context.INPUT_SERVICE, inputManager);
        ...
        mActivityManagerService.systemReady(new Runnable() {
            @Override
            public void run() {
                ...
        });
```
这个方法启动了众多的service，比如我们经常用到的WindowManagerService和InputManagerService，因为启动的服务太多，这里就不一一列举了，在方法的最后调用了ActivityManagerService的systemReady方法
####step 33.systemReady
```
    public void systemReady(final Runnable goingCallback) {
            ...
            startHomeActivityLocked(mCurrentUserId, "systemReady");
            ...
}
```
systemReady方法里也做了许多其他的工作，如检查升级，发送启动完成的广播等，最主要的是在最后调用startHomeActivityLocked方法，用来启动系统的桌面
####step 34.startHomeActivityLocked
```
boolean startHomeActivityLocked(int userId, String reason) {
        ...
        
        Intent intent = getHomeIntent();
         ...
        mStackSupervisor.startHomeActivity(intent, aInfo, reason);
         ...    
    
    }
```
```
Intent getHomeIntent() {
        Intent intent = new Intent(mTopAction, mTopData != null ? Uri.parse(mTopData) : null);
        intent.setComponent(mTopComponent);
        if (mFactoryTest != FactoryTest.FACTORY_TEST_LOW_LEVEL) {
            intent.addCategory(Intent.CATEGORY_HOME);
        }
        return intent;
    }
```
这里getHomeIntent加了一个Intent.CATEGORY_HOME的Category，系统的Launcher这个Activity正好有这个android.intent.category.HOME的Category，这个文件在[/packages/apps/Launcher2/AndroidManifest.xml](http://androidxref.com/6.0.1_r10/xref/packages/apps/Launcher2/AndroidManifest.xml)
```
 <application
        android:name="com.android.launcher2.LauncherApplication"
        android:label="@string/application_name"
        android:icon="@mipmap/ic_launcher_home"
        android:hardwareAccelerated="true"
        android:largeHeap="@bool/config_largeHeap"
        android:supportsRtl="true">
        <activity
            android:name="com.android.launcher2.Launcher"
            android:launchMode="singleTask"
            android:clearTaskOnLaunch="true"
            android:stateNotNeeded="true"
            android:resumeWhilePausing="true"
            android:theme="@style/Theme"
            android:windowSoftInputMode="adjustPan"
            android:screenOrientation="nosensor">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.HOME" />
                <category android:name="android.intent.category.DEFAULT" />
                <category android:name="android.intent.category.MONKEY"/>
            </intent-filter>
        </activity>

```
所以这里是隐式的启动了Launcher，接着调用mStackSupervisor的startHomeActivity方法
####step 35.mStackSupervisor.startHomeActivity
ActivityStackSupervisor([/frameworks/base/services/core/java/com/android/server/am/ActivityStackSupervisor.java](http://androidxref.com/6.0.1_r10/xref/frameworks/base/services/core/java/com/android/server/am/ActivityStackSupervisor.java))
```
 void startHomeActivity(Intent intent, ActivityInfo aInfo, String reason) {
        ...
        startActivityLocked(null /* caller */, intent, null /* resolvedType */, aInfo,
                null /* voiceSession */, null /* voiceInteractor */, null /* resultTo */,
                null /* resultWho */, 0 /* requestCode */, 0 /* callingPid */, 0 /* callingUid */,
                null /* callingPackage */, 0 /* realCallingPid */, 0 /* realCallingUid */,
                0 /* startFlags */, null /* options */, false /* ignoreTargetSecurity */,
                false /* componentSpecified */,
                null /* outActivity */, null /* container */,  null /* inTask */);
       
    }
```
这里调用了startActivityLocked方法，其实这个方法就是我们平时调用startActivity方法之后会调用的方法了，至此桌面就被启动起来了

# 小结
步骤24-35讲解从SystemServer到系统桌面的过程，这之中SystemServer比较关键，里面启动了系统必须的所有服务，比如ActivityManagerService,WindowMangerService等，在ActivityManagerService的SystemReady方法中启动了桌面Activity Luncher

# 总结
从开机到启动桌面，大概过程如下：
* 1.启动linux内核，fork第一个进程init
* 2.init进程解析init.rc文件，然后fork出众多进程，zygote进程是其中之一
* 3.zygote进程启动的虚拟机，从C++环境切换到Java环境，并fork出system_server进程
* 4.SystemServer启动众多的系统Service如ActivityManagerService,WindowMangerService等
* 5.ActivityManagerService启动桌面Activity Luncher