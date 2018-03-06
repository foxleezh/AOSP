# Android 源码分析

>关于我

- foxleezh
- [我的博客](http://foxleezh.me)
- [github](https://github.com/foxleezh/)
- [掘金](https://juejin.im/user/57406ad279df540060555258)
- [简书](http://www.jianshu.com/users/b1eec1cd9bfd)
- [邮箱-foxleezh@gmail.com](foxleezh@gmail.com)

## 前言

AOSP的源码是非常庞大的，里面的语言主要有C/C++，Java, 汇编，为了让大家能有更好的阅读体验，我专门写了篇文章作为导读

[如何阅读Android源码](https://github.com/foxleezh/AOSP/issues/2)<br>

## 系统结构

Android系统结构很复杂，可能大家平常都是应用层开发比较多，我们开发的app属于最上层

应用层下面有个应用框架层（framework）,也就是我们经常在Android Studio中可以直接点击看到那些源码，应用层和应用框架层都是用Java写的

在应用框架层下面是Native层，也就是我们调用的native方法的实现层，这一层主要是用C/C++写的

在Native层再往下就是Linux内核层，这一层主要是用C和汇编来写的

在Native层和Linux内核层之间还有个硬件抽象层，准确讲它应该属于Linux内核层，因为都是些驱动相关的
但是因为Linux是开源的，如果放在Linux内核层就必须开源代码，为了保护厂商的驱动源码，所以将这些代码专门提出来，放到了一个硬件抽象层

可以看到，Android系统用了Java,C/C++，汇编，这些不同的语言和层级是如何打通的呢？这里主要涉及到JNI和Syscall，
我们知道Java是运行在虚拟机中的，在Native层就有一个专门的虚拟机，dex代码和so动态链接库都会加载到这个虚拟机中，
使用同一个进程空间，dex代码和so动态链接库之间定义的相同的接口，就像我们平时写服务器和客户端用相同的接口字段一样，
这样Java和C/C++之间就可以相互通信了，而这套机制就叫JNI（Java Native Interface）

Native层是运行在用户空间的，Linux内核层是运行在内核空间，一般情况下，用户进程是不能访问内核的，
它既不能访问内核所在的内存空间，也不能调用内核中的函数. 而Syscall就是专门用来让Native访问Linux内核的，
在/platform/bionic/libc/kernel/uapi/asm-generic/unistd.h中，在这个文件中为每一个系统调用规定了唯一的编号，叫做系统调用号
```C
#define __NR_epoll_create1 20
#define __NR_epoll_ctl 21
#define __NR_epoll_pwait 22
#define __NR_dup 23
```
这里面每一个宏就是一个系统调用号,每一个调用号都会对应Linux内核的一个操作.
Syscall是单向的，只能是Native调用Linux内核，
JNI却是双向的，Java可以调用C++,C++也可以调用Java.那么Linux内核如何调用Native呢，其实也很简单，
内核直接运行一个可执行程序就可以了,比如native中的init进程就是这样调用的

## 通信方式
Android系统中有许多通信方式，最常见就是Binder和Handler，这是我们平常开发中用到的，另外底层常见的还有Socket, pipe，signal
除了Handler只能用于线程间通信外，其他都可以进行进程间通信，当然你也可以用来做线程间通信，只是有点杀鸡用牛刀的感觉.

Binder作为Android系统提供的一种IPC机制，无论从系统开发还是应用开发，都是Android系统中最重要的组成. Binder通信采用c/s架构，从组件视角来说，包含Client、Server、ServiceManager以及binder驱动，
其核心实现原理是用系统调用ioctl在内核空间进行进程间通信，它还做了许多良好的封装，比如如果发现调用者和接收者是同一进程，就不会去走系统调用,而是直接调用

Handler消息机制我们比较熟悉，Handler消息机制是由一组MessageQueue、Message、Looper、Handler共同组成的，为了方便且称之为Handler消息机制.
其核心实现原理是共享内存，由于工作线程与主线程共享地址空间，即Handler实例对象mHandler位于线程间共享的内存堆上，工作线程与主线程都能直接使用该对象.
Handler最经典的地方就是消息队列，MessageQueue存放要处理的消息Message，Looper无限循环从MessageQueue中取Message发到对应线程处理

## 内容分类

本项目以android-8.0.0_r17和kernel/msm(高通内核android-8.0.0_r0.16)为基础，重点分析跟应用程序相关的源码，主要内容如下：
- Android系统启动流程，应用启动流程，四大组件启动流程，这将列入系统启动篇
- 系统常用服务ActivityManagerService,WindowManagerService等，这将列入系统服务篇
- 通信机制，主要是Binder和Handler，这将列入通信篇
- 进程和线程的创建，运行，销毁，这将列入进程篇
- View的绘制和显示流程，事件分发机制，这将列入图形绘制篇
- Android虚拟机ART运行机制，类加载机制，Java注解，Java反射，这将列入虚拟机篇
- Android对于Java集合的优化算法，这将列入Java基础篇

我将持续更新本项目，尽可能地为大家提供透彻的Android源码分析，每篇文章我会挂在issue上，方便大家探讨并提出问题
### 工具篇
- [如何下载Android源码](https://github.com/foxleezh/AOSP/issues/1)<br>
- [如何阅读Android源码](https://github.com/foxleezh/AOSP/issues/2)<br>
- [C语言知识整理](https://github.com/foxleezh/AOSP/issues/4)<br>
- [C++语言知识整理](https://github.com/foxleezh/AOSP/issues/7)<br>
- [Linux常见内核函数](https://github.com/foxleezh/AOSP/issues/5)<br>
- [Andorid常见内核函数](https://github.com/foxleezh/AOSP/issues/8)<br>

### 系统启动篇
- [Android系统启动流程之Linux内核](https://github.com/foxleezh/AOSP/issues/3)<br>
- [Android系统启动流程之init进程(一)](https://github.com/foxleezh/AOSP/issues/6)<br>
- [Android系统启动流程之init进程(二)](https://github.com/foxleezh/AOSP/issues/9)<br>
