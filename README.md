# Android 源码分析

>关于我

- foxleezh
- [我的博客](http://foxleezh.me)
- [github](https://github.com/foxleezh/)
- [掘金](https://juejin.im/user/57406ad279df540060555258)
- [简书](http://www.jianshu.com/users/b1eec1cd9bfd)
- [邮箱-foxleezh@gmail.com](foxleezh@gmail.com)

## 前言

本项目以android-8.0.0_r17(目前最新版本)和kernel/msm(高通内核android-8.0.0_r0.16)为基础，重点分析跟应用程序相关的源码，主要内容如下：
- Android系统启动流程，应用启动流程，四大组件启动流程，这将列入系统启动篇
- 系统常用服务ActivityManagerService,WindowManagerService等，这将列入系统服务篇
- 通信机制，主要是Binder和Handler，这将列入通信篇
- 进程和线程的创建，运行，销毁，这将列入进程篇
- View的绘制和显示流程，事件分发机制，这将列入图形绘制篇
- Android虚拟机ART运行机制，类加载机制，Java注解，Java反射，这将列入虚拟机篇
- Android对于Java集合的优化算法，这将列入Java基础篇

我将持续更新本项目，尽可能地为大家提供透彻的Android源码分析，每篇文章我会挂在issue上，方便大家探讨并提出问题
## 一、工具篇
- [如何下载Android源码](https://github.com/foxleezh/AOSP/issues/1)<br>
- [如何阅读Android源码](https://github.com/foxleezh/AOSP/issues/2)<br>

## 二、系统启动篇
- [Android系统启动流程之Linux内核](https://github.com/foxleezh/AOSP/issues/3)<br>