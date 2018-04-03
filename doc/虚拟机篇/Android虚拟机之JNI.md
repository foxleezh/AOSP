## 前言
前文讲到虚拟机创建后反射调用了ZygoteInit的main方法，说到虚拟机，我们就不得不说下JNI，它是沟通Java和C++的桥梁。
JNI全称是Java Native Interface,可以把它理解为一种接口编程方式，就像我们平常开发的C/S模式一样，
Client和Server要通信，那就得用接口。JNI主要包括两个方面的内容：

- C++调用Java
- Java调用C++

本文涉及到的文件
```
platform/libnativehelper/include/nativehelper/jni.h
platform/art/runtime/java_vm_ext.cc
platform/art/runtime/jni_internal.cc
platform/frameworks/base/core/java/com/android/internal/os/ZygoteInit.java
platform/libcore/dalvik/src/main/java/dalvik/system/ZygoteHooks
platform/art/runtime/native/dalvik_system_ZygoteHooks.cc
platform/art/runtime/runtime.h
platform/libnativehelper/JNIHelp.cpp
platform/libcore/luni/src/main/java/android/system/Os.java
platform/libcore/luni/src/main/java/libcore/io/Libcore.java
platform/libcore/luni/src/main/java/libcore/io/BlockGuardOs.java
platform/libcore/luni/src/main/java/libcore/io/ForwardingOs.java
platform/libcore/luni/src/main/java/libcore/io/Linux.java
platform/libcore/luni/src/main/native/libcore_io_Linux.cpp
```

## 一、C++调用Java

为什么我先讲C++调用Java呢？因为前文创建了虚拟机后，首先是从C++调用了Java，所以我接着前文的例子来讲,
我们回顾一下之前C++调用ZygoteInit的main函数的过程,我将分段一步步为大家解释。

```C
void AndroidRuntime::start(const char* className, const Vector<String8>& options, bool zygote)
{
    /*
     * We want to call main() with a String array with arguments in it.
     * At present we have two arguments, the class name and an option string.
     * Create an array to hold them.
     */
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

    ...

}
```

### 1.1 Java中各类型在C++的对应关系

比如说我们Java中有常见的Class，String,int,short等，这些在C++中并不是叫原来的名字，而是另外取了个名字，
基本就是在原来的名字前加了个j，表示java. 下面是他们的对应关系

基本数据类型和void

|Java类型|C++类型|
| :-- | :-- |
|boolean|	jboolean|
|byte	|jbyte	|
|char	|jchar	|
|short	|jshort	|
|int	|jint	|
|long	|jlong	|
|float	|jfloat |
|double	|jdouble|
|void	|void	|

引用数据类型

|Java类型|C++类型|
| :-- | :-- |
|All objects|	jobject|
|java.lang.Class实例	|jclass	|
|java.lang.String实例|jstring	|
|java.lang.Throwable实例	|jthrowable	|
|Object[]（包含Class,String,Throwable）| jobjectArray	|
|boolean[]	|jbooleanArray	|
|byte[]（其他基本数据类型类似）|jbyteArray|


那其实下面的代码就好理解了,就相当于定义了三个局部变量，类型为Class,String[],String
```C
    jclass stringClass;
    jobjectArray strArray;
    jstring classNameStr;
```

### 1.2 env->FindClass

我们再接着往下看，env->FindClass, env是虚拟机的环境，可以类比为Android中无处不在的Context,
但是这个env是指特定线程的环境，也就是说一个线程对应一个env. <br>

env有许多的函数，FindClass只是其中一个，作用就是根据ClassName找到对应的class，
用法是不是跟Java中反射获取Class有点像,其实Java反射也是native方法，而在实现上也是跟env->FindClass一样.

我们来具体看看env->FindClass的实现,env的类型是JNIEnv,定义在platform/libnativehelper/include/nativehelper/jni.h中,
这个JNIEnv 在C环境和C++环境类型不一样，在C环境中定义的是JNINativeInterface* ，
而C++中定义的是_JNIEnv，_JNIEnv其实内部也是调用JNINativeInterface的对应函数，只是做了层代理,
JNINativeInterface是个结构体，里面就有我们要找的函数FindClass

```C
#if defined(__cplusplus) //如果是C++
typedef _JNIEnv JNIEnv;
typedef _JavaVM JavaVM;
#else //如果是C
typedef const struct JNINativeInterface* JNIEnv;
typedef const struct JNIInvokeInterface* JavaVM;
#endif 


struct _JNIEnv {
    const struct JNINativeInterface* functions;
    ...
    jclass FindClass(const char* name)
    { return functions->FindClass(this, name); } 
    ...
}


struct JNINativeInterface { 
    ...
    jclass      (*FindClass)(JNIEnv*, const char*);
    ...
}

```

那这个结构体JNINativeInterface中FindClass的函数指针什么时候赋值的呢？还记得上文中有个创建虚拟机的函数JNI_CreateJavaVM,
里面有个参数就是JNIEnv,其实也就是在创建虚拟机的时候把函数指针赋值的，我们知道JNI_CreateJavaVM是加载libart.so时获取的，
那我们就得找libart.so的源码，这个对应的源码在platform/art/runtime/java_vm_ext.cc,它会调用Runtime::Create函数去新建
线程，在线程新建的过程中会对JNIEnv进行赋值，JNI_CreateJavaVM函数最后会去调用线程的GetJniEnv得到JNIEnv的实例，将实例赋值给p_env.


(线程在新建过程中如何对JNIEnv进行赋值的，就不细讲了，我提供几个关键的函数，runtime.cc的Create和Init、thread.cc的Attach和Init、
jni_env_ext.cc的Create、jni_internal.cc的GetJniNativeInterface，涉及到的文件我都放在AOSP项目中，有兴趣的可以去看看. )

```C
extern "C" jint JNI_CreateJavaVM(JavaVM** p_vm, JNIEnv** p_env, void* vm_args) {
  ...

  if (!Runtime::Create(options, ignore_unrecognized)) {
    return JNI_ERR;
  }

  *p_env = Thread::Current()->GetJniEnv();
}
```

GetJniEnv返回的是一个JNINativeInterface的实例,定义在/platform/art/runtime/jni_internal.cc，其中就有我们要找的FindClass

```C
const JNINativeInterface gJniNativeInterface = {
  nullptr,  // reserved0.
  nullptr,  // reserved1.
  nullptr,  // reserved2.
  nullptr,  // reserved3.
  JNI::GetVersion,
  JNI::DefineClass,
  JNI::FindClass,
}
```
我们看到实例中FindClass对应的函数是JNI::FindClass，定义在当前文件中,FindClass的工作是交给ClassLinker，
ClassLinker内部的实现是通过ClassLoader获取一个ClassTable对象，再通过ClassTable中的一个HashSet得到对应的Class,
ClassLoader其实我们也比较熟悉，Java层中就有，我们apk中的dex文件就是需要ClassLoader去加载，最终会将Class装进一个HashSet中，
因此，我们FindClass也去这个HashSet中去找.

(ClassLinker内部的实现我就不细讲了，我提供几个关键的函数，class_linker.cc的FindClass和LookupClass、class_table.cc的Lookup
，涉及到的文件我都放在AOSP项目中，有兴趣同学可以去具体看看.)

```C
  static jclass FindClass(JNIEnv* env, const char* name) {
    CHECK_NON_NULL_ARGUMENT(name);
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker(); //获取ClassLinker
    std::string descriptor(NormalizeJniClassDescriptor(name));
    ScopedObjectAccess soa(env);
    mirror::Class* c = nullptr;
    if (runtime->IsStarted()) {
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::ClassLoader> class_loader(hs.NewHandle(GetClassLoader(soa)));
      c = class_linker->FindClass(soa.Self(), descriptor.c_str(), class_loader); //查找类
    } else {
      c = class_linker->FindSystemClass(soa.Self(), descriptor.c_str()); //查找系统类
    }
    return soa.AddLocalReference<jclass>(c);
  }
```


说完env->FindClass，其实其他env->方式调用的函数也就大体知道源码在哪儿了，在接下来的分析中我就只说明下对应函数的作用，具体实现可以根据
自己的需要深入去看.

### 1.3 其他env函数

env函数特别多，我这里只列举一些我们常用的

新建实例，相当于Java中的new

|函数名|作用|类比Java|
| :-- | :-- | :-- |
|NewObject|新建Object|new Object|
|NewStringUTF|新建String字符|new String()|
|NewObjectArray|新建Object数组|new Object[]|
|New(Type)Array|新建Type数组,如NewByteArray|new byte[]|

获取和设置成员变量和类变量，相当于Java中的获取和设置变量，下面以A a=new A()为例子

|函数名|作用|类比Java|
| :-- | :-- | :-- |
|GetFieldID|获取成员变量id,所有获取成员变量的方法都要传入这个值|--|
|GetObjectField|获取Object类型的成员变量|a.object|
|Get(Type)Field|获取Type类型的成员变量,如GetBooleanField|bool b=a.bool|
|Set(Type)Field|设置Type类型的成员变量,如SetBooleanField|a.bool=b|
|GetStaticFieldID|获取类变量id,所有获取类变量的方法都要传入这个值|--|
|GetStaticObjectField|获取Object类型的类变量|A.object|
|GetStatic(Type)Field|获取Type类型的类变量,如GetStaticBooleanField|bool b=A.bool|
|SetStatic(Type)Field|设置Type类型的类变量,如SetStaticBooleanField|A.bool=b|

调用成员方法和类方法，相当于Java中的调用方法，下面以A a=new A()为例子

|函数名|作用|类比Java|
| :-- | :-- | :-- |
|GetMethodID|获取成员方法id,所有获取成员方法的方法都要传入这个值|--|
|CallObjectMethod|调用返回值为Object类型的成员方法|Object o=a.a()|
|Call(Type)Method|调用返回值为Type类型的成员方法,如CallBooleanMethod|bool b=a.b()|
|GetStaticMethodID|获取类方法id,所有获取类方法的方法都要传入这个值|--|
|CallStaticObjectMethod|调用返回值为Object类型的类方法|Object o=A.a()|
|CallStatic(Type)Method|调用返回值为Type类型的类方法,如CallStaticBooleanMethod|bool b=A.b()|

数组相关操作,以bool[] bs=new bool[] 为例

|函数名|作用|类比Java|
| :-- | :-- | :-- |
|Get(Type)ArrayElements|获取Type类型的数组的某个元素|bool b=bs[0]|
|Set(Type)ArrayElements|设置Type类型的数组的某个元素|bs[0]=b|

内存释放相关，这个是C++独有的，没有Java相应的调用

|函数名|作用|类比Java|
| :-- | :-- | :-- |
|ReleaseStringUTFChars|释放String|--|
|Release(Typge)ArrayElements|释放Type类型的数组|--|

我这里只是笼统地列举了一些env函数的作用，对于参数及返回值并没有细讲，主要是这些属于API范畴的东西，要用的时候再查也不迟，
我推荐几个网址吧，[英文官方](https://docs.oracle.com/javase/1.5.0/docs/guide/jni/spec/jniTOC.html),
[中文手册](https://blog.csdn.net/darmao/article/details/70139100?locationNum=3&fps=1)

### 1.4 函数签名

start函数最后会调用main函数，在获取main函数时需要传递三个参数，第一个是函数所在的类，第二个是函数名称，第三个就是函数签名

```C
   jmethodID startMeth = env->GetStaticMethodID(startClass, "main", "([Ljava/lang/String;)V");
```

函数签名其实就是对一个函数的参数及返回值的一种符号表示，表示形式是 (params)return 下面我列举一下符号与Java类型的一一对应关系：

基本数据类型和void,我们可以看到除了boolean和long表示得不一样外，其他都是以首字母进行表示，我想主要原因可能是B与byte冲突了，L与object冲突

|符号|Java类型|
| :-- | :-- |
|B |byte|
|C |char|
|S |short|
|I |int|
|F |float|
|D |double|
|Z |boolean|
|J |long|
|V |void|

引用数据类型和数组，引用数据类型以L开头，后面接完整路径，最后有个分号，这个分号一定不要忘记！一定不要忘记！一定不要忘记！

|符号|Java类型|
| :-- | :-- |
|L/java/lang/String;|String|
|[I|int[]|
|[L/java/lang/object;|object[]|

我们回到刚才的例子 ([Ljava/lang/String;)V ，这个就表示main函数的参数是String[],返回值是void.

### 1.5 异常处理

我们在Java中经常用try catch来处理异常非常方便，我们在C++中调用Java函数时，也可以去捕获异常，我们可以有两种方式：

- ExceptionCheck
- ExceptionOccurred

我先讲讲 ExceptionCheck ，这个函数是会返回一个bool值，true表示有异常，false表示没有异常

```C
    env->CallStaticVoidMethod(cls,mid);
    if (env->ExceptionCheck()) {  // 检查JNI调用是否有引发异常
        env->ExceptionDescribe(); //打印错误日志堆栈信息
        env->ExceptionClear(); // 清除引发的异常
        env->ThrowNew(env->FindClass(env,"java/lang/Exception"),"JNI抛出的异常！"); //抛出异常
    }
```

再看看ExceptionOccurred，这个用法其实跟ExceptionCheck差不多,只是它返回的不是bool值，而是当前异常的引用

```C
jthrowable exc = NULL;
exc = env->ExceptionOccurred();  // 返回一个指向当前异常对象的引用
if (exc) {
    env->ExceptionDescribe(); //打印错误日志堆栈信息
    env->ExceptionClear(); // 清除引发的异常
    env->ThrowNew(env->FindClass(env,"java/lang/Exception"),"JNI抛出的异常！"); //抛出异常
}
```


## 二、Java调用C++

讲完了C++调用Java，我们再看看Java如何调用C++,我们接着前面的讲，之前通过 env->CallStaticVoidMethod(startClass, startMeth, strArray)
调用了ZygoteInit的 main 函数，我们就以main函数为例讲解Java调用C++的过程。

### 2.1 main
定义在platform/frameworks/base/core/java/com/android/internal/os/ZygoteInit.java

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

        ...

}

```

main函数最开始new了一个ZygoteServer，这个后续会用到，然后设置标记，不允许新建线程，为什么不允许多线程呢？
这主要是担心用户创建app时，多线程情况下某些预先加载的资源没加载好，这时去调用会出问题. 接着设置了zygote进程的进程组id，
最后便是一系列性能统计相关的动作

#### 4.1.1 startZygoteNoThreadCreation
定义在platform/libcore/dalvik/src/main/java/dalvik/system/ZygoteHooks中

```java

    /*
     * Called by the zygote when starting up. It marks the point when any thread
     * start should be an error, as only internal daemon threads are allowed there.
     */
    public static native void startZygoteNoThreadCreation();
```

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
好像有点苗头了，gMethods数组中有我们要的startZygoteNoThreadCreation，这个数组的类型是JNINativeMethod，但是数组中却是
NATIVE_METHOD，我们看看这个NATIVE_METHOD是什么
```C
#define NATIVE_METHOD(className, functionName, signature) \
{ #functionName, signature, (void*)(className ## _ ## functionName) }
```
如何理解这个定义呢？#define是宏定义，也就是说编译期间要做宏替换，这里就是把NATIVE_METHOD替换成
{"","",(void*)()},具体怎么替换呢？我们看到{}里有些#、##，#表示字符串化，##表示字符串化拼接，相当于Java中的
String.format,以NATIVE_METHOD(ZygoteHooks, startZygoteNoThreadCreation, "()V")为例，替换后就是
{"startZygoteNoThreadCreation","()V",(void*)(ZygoteHooks_startZygoteNoThreadCreation) }

我们再回顾下 JNINativeMethod ，它是一个结构体,name表示native函数名，signature表示用字符串描述native函数的参数和返回值,
fnPtr表示native指向的C++函数指针,这其实就是动态注册的映射关系了，将native函数对应一个C++函数
```C
typedef struct {
const char* name;
const char* signature;
void* fnPtr;
} JNINativeMethod;
```

JNINativeMethod只是个结构体，真正注册的函数是在 REGISTER_NATIVE_METHODS("dalvik/system/ZygoteHooks")，我们先看看
REGISTER_NATIVE_METHODS
```c
#define REGISTER_NATIVE_METHODS(jni_class_name) \
  RegisterNativeMethods(env, jni_class_name, gMethods, arraysize(gMethods))
```
它也是一个宏定义，指向的是RegisterNativeMethods，这个函数定义在platform/frameworks/base/core/jni/AndroidRuntime.cpp
```C
/*
 * Register native methods using JNI.
 */
/*static*/ int AndroidRuntime::registerNativeMethods(JNIEnv* env,
    const char* className, const JNINativeMethod* gMethods, int numMethods)
{
    return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}
```
其实它调用的是jniRegisterNativeMethods，这个定义在platform/libnativehelper/JNIHelp.cpp，
jniRegisterNativeMethods函数首先是将传过来的类名字符串找到对应的class，然后就是调用(*env)->RegisterNatives动态注册JNI，
其实调用这么多层，动态注册最关键的就是构建一个结构体JNINativeMethod，然后调用(*env)->RegisterNatives，RegisterNatives属于
虚拟机内的函数了，今后讲虚拟机时我再具体去分析，这里我们知道它的作用就行了.
```C
extern "C" int jniRegisterNativeMethods(C_JNIEnv* env, const char* className,
    const JNINativeMethod* gMethods, int numMethods)
{
    JNIEnv* e = reinterpret_cast<JNIEnv*>(env);

    ALOGV("Registering %s's %d native methods...", className, numMethods);

    scoped_local_ref<jclass> c(env, findClass(env, className)); //根据类名找到class
    if (c.get() == NULL) {
        char* tmp;
        const char* msg;
        if (asprintf(&tmp,
                     "Native registration unable to find class '%s'; aborting...",
                     className) == -1) {
            // Allocation failed, print default warning.
            msg = "Native registration unable to find class; aborting...";
        } else {
            msg = tmp;
        }
        e->FatalError(msg);
    }

    if ((*env)->RegisterNatives(e, c.get(), gMethods, numMethods) < 0) { //动态注册jni
        char* tmp;
        const char* msg;
        if (asprintf(&tmp, "RegisterNatives failed for '%s'; aborting...", className) == -1) {
            // Allocation failed, print default warning.
            msg = "RegisterNatives failed; aborting...";
        } else {
            msg = tmp;
        }
        e->FatalError(msg);
    }

    return 0;
}
```

我们接着上面的startZygoteNoThreadCreation函数讲，由上可知这个native函数实际会调用ZygoteHooks_startZygoteNoThreadCreation,
它定义在platform/art/runtime/native/dalvik_system_ZygoteHooks.cc
```C
static void ZygoteHooks_startZygoteNoThreadCreation(JNIEnv* env ATTRIBUTE_UNUSED,
                                                    jclass klass ATTRIBUTE_UNUSED) {
  Runtime::Current()->SetZygoteNoThreadSection(true);
}
```
其实它又是调用Runtime的SetZygoteNoThreadSection函数，这个定义在platform/art/runtime/runtime.h,这个函数的实现非常简单，
就是将zygote_no_threads_这个bool值设置为想要的值
```C
static Runtime* instance_;

// Whether zygote code is in a section that should not start threads.
bool zygote_no_threads_;

static Runtime* Current() {
   return instance_;
}

void SetZygoteNoThreadSection(bool val) {
   zygote_no_threads_ = val;
}

```

由此我们可以看到startZygoteNoThreadCreation这个native函数经过层层调用，最终就是将一个bool变量设置为true. 讲得是有点多了，
这里主要是告诉大家如何去追踪native函数的实现，因为这是阅读frameworks层代码必备的技能. 这里我还是再次推荐大家用Source Insight
来看代码，不管是函数跳转还是全局搜索都是非常方便的，详情请看我之前写的[如何阅读Android源码](https://github.com/foxleezh/AOSP/issues/2)

#### 4.1.2 setpgid
定义在platform/libcore/luni/src/main/java/android/system/Os.java

这个Os.java类是比较特殊的一个类，这个类相当于一个代理类，所有的方法都是去调用Libcore.os类中相关的方法，

```java
 /**
   * See <a href="http://man7.org/linux/man-pages/man2/setpgid.2.html">setpgid(2)</a>.
   */
  /** @hide */ public static void setpgid(int pid, int pgid) throws ErrnoException { Libcore.os.setpgid(pid, pgid); }

```
而Libcore.os的实现类是BlockGuardOs，BlockGuardOs的父类是ForwardingOs，ForwardingOs也是个代理类，里面所有方法都是调用
Linux.java中的对应函数，也就是说Os.java中的函数最终调用的是Linux.java中的函数. 另外在BlockGuardOs类中有重载一些方法，做了一些
Policy权限的检查.
```java
public final class Libcore {
    private Libcore() { }

    /**
     * Direct access to syscalls. Code should strongly prefer using {@link #os}
     * unless it has a strong reason to bypass the helpful checks/guards that it
     * provides.
     */
    public static Os rawOs = new Linux();

    /**
     * Access to syscalls with helpful checks/guards.
     */
    public static Os os = new BlockGuardOs(rawOs);
}
 
```
我们再来看看Linux.java的实现是怎样的
```java
public final class Linux implements Os {
    Linux() { } 

    ...
    public native void setpgid(int pid, int pgid) throws ErrnoException;
    ...
}
```
没错，这里面全是native函数，这些native的实现又在哪儿呢？老方法，找libcore_io_Linux，果然又找到了libcore_io_Linux.cpp

```C
static JNINativeMethod gMethods[] = {
    ...

    NATIVE_METHOD(Linux, setpgid, "(II)V"),

    ...
}

void register_libcore_io_Linux(JNIEnv* env) {
    jniRegisterNativeMethods(env, "libcore/io/Linux", gMethods, NELEM(gMethods));
}

static void Linux_setpgid(JNIEnv* env, jobject, jint pid, int pgid) {
    throwIfMinusOne(env, "setpgid", TEMP_FAILURE_RETRY(setpgid(pid, pgid)));
}
```

注册方式也是跟之前一样，用jniRegisterNativeMethods，由此我们知道setpgid就是调用Linux的系统调用setgpid.
这个系统调的作用是设置进程组id，第一个参数pid是指设置哪个进程所属的进程组，如果是0,就是当前进程所属的进程组，第二个参数是设置的id值，
如果是0,那么就把当前进程的pid作为进程组的id. 所以setgpid（0,0）的意思就是将zygote进程所在进程组id设置为zygote的pid



