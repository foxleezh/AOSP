## 前言
当我们把源码下载下来之后，会感到茫然无措，因为AOSP的源码实在是太多了，这里我们需要明确一些问题：

* 要阅读哪些源码
* 阅读源码的顺序和方式
* 用什么工具来阅读

下面我将从这三个问题一一展开

## 一、要阅读哪些源码

这个问题是比较个性化的，因为不同的人从事着不同的工作，有的人从事应用开发，可能对Java层东西感兴趣;有的人从事Framework开发，可能对Framework层感兴趣;有的从事硬件开发，可能对底层实现感兴趣。

这个都因人而异，但是有一点，不能盲目地毫无目的地看源码，因为这样的话最终你会淹没在AOSP的大海里，看了一年半截啥都看了，却又感觉都没看透，别人问你源码的东西，都能说个一二，但是一往深了说，就不知所以了。

所以对于AOSP源码，不在于多，而在于精，你不要试图把所有的源码都看懂，你只要对自己感兴趣的那部分深入研究就可以，因为即便是Google工程师也不可能把AOSP全部读完。

对于我而言，我是从事应用层开发的，我主要会了解以下几个方面的源码：

* Android系统启动流程，应用启动流程，四大组件启动流程，这将列入系统启动篇
* 系统常用服务ActivityManagerService,WindowManagerService等，这将列入系统服务篇
* 通信机制，主要是Binder和Handler，这将列入通信篇
* 进程和线程的创建，运行，销毁，这将列入进程篇
* View的绘制和显示流程，事件分发机制，这将列入图形绘制篇
* Android虚拟机ART运行机制，类加载机制，Java注解，Java反射，这将列入虚拟机篇
* Android对于Java集合的优化算法，这将列入Java基础篇

## 二、阅读源码的顺序和方式

#### 2.1 阅读顺序
读源码是一个日积月累的过程，不可能一蹴而就，当我们列出自己感兴趣的源码后，我们需要制定一个阅读计划，先读什么再读什么。这个也是因人而异，根据自己的兴趣来就是，你最想读什么，那就排前面。

我一直在说兴趣，因为兴趣是最好的老师，只有你对一样东西感兴趣了，才会有动力去学，去研究，才会不觉得累，如果一开始就去啃一些你不感兴趣的东西，到头来也是乏味不专注的，理解的程度也是不深，而且很有可能失去信心，最后放弃阅读。

当然，如果你对好几样东西都感兴趣，那就有一些原则了：

* 事物都讲究先后，就像树木扎根大地一样，先有大地，才有树木，基础的东西先看
* 相互有关联的东西一起看，不要一会儿看系统启动，突然又去看事件分发什么的

#### 2.2 阅读方式
Android系统涵盖的范围很广，从上层的应用程序，到Framework，再到Libraries以至硬件，从Java层到C++，就像一座几十层的大厦一样，每层都有楼梯，也有电梯，我们需要做的就是在大厦里上下穿梭。

当我们阅读某一个知识点源码的时候，不同的知识点有不同的阅读方式，有些适合从下往上读，比如系统启动流程，我是从事件开始的地方开始读，从init.cpp开始，然后到zygote进程，到Java虚拟机，最后到Luncher；

有些适合从上往下读，比如Activity的启动，我是从startActivity方法开始读，然后到ActivityThread，然后到ActivityManagerService;

有些适合两头从中间读，比如Binder,我是从Java层看到C++层，但是看到驱动那儿看不动了，然后就从接收Binder的地方往回看，最后在两端集中在驱动的地方前后对比，才将Binder看通。

这里还是有个好的方式，就是从事件触发的地方开始看是比较合适的。

## 三、用什么工具来阅读

Android 源码阅读神器当然是Source Insight 
![](https://user-images.githubusercontent.com/7986735/31375054-e5dba386-add2-11e7-80e0-aa518b10c648.gif)

Source Insight的好处：

* 支持方法跳转，类跳转，并且对C++支持很好
* 支持文件搜索，java,c++，xml都支持，并且支持内容搜索
* 支持一键导入，随时配置路径
* 而且最重要的，无论文件有多少，一点都不卡！

下面我讲讲如何使用Source Insight
#### 1、下载安装Source Insight

下载地址 http://download.csdn.net/download/foxlee1991/9882553 ，我还专门配置了一个跟Android Studio一样的Darcula主题，下载地址 http://download.csdn.net/download/foxlee1991/9882535

#### 2、导入AOSP源码

我目前还没有下载完整的AOSP源码，只是先下载了几个重要的源码。打开Source Insight,选择Project -> New Project，取个名字比如叫AOSP，点击OK

![](http://upload-images.jianshu.io/upload_images/3387045-8412a62d1b79a699.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

选择你要查看的源码目录，点击OK

![](http://upload-images.jianshu.io/upload_images/3387045-c901bed5d8670ddb.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

选择需要将哪些目录下的源码导入，点击Add Tree

![](http://upload-images.jianshu.io/upload_images/3387045-5033e5e91f286fa5.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

导入成功后会有很多文件列在下方，点击Close

![](http://upload-images.jianshu.io/upload_images/3387045-cf873591e37eadb3.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)


#### 3、查看源码
现在进入项目还是一片空白，需要把工具栏打开，然后就可以看源码了

![](http://upload-images.jianshu.io/upload_images/3387045-c80e01b4511ba549.gif?imageMogr2/auto-orient/strip)

左边是方法和成员变量搜索，右边Project File是搜索类名，Project Symbol是内容搜索

![](http://upload-images.jianshu.io/upload_images/3387045-0a23a2fc65603414.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)


还有一些快捷键，比如Ctrl+左键可以方法跳转，左上角有前进和后退，Ctrl+G 是跳转到指定行，Ctrl+F 搜索内容

如果你习惯用Android Studio来看源码也是可以的，但是它有两个弊端，一是对C++代码支持不太好，二是如果源码太多会很卡。

所以我这里会把我阅读的一些源码拷贝到本项目中，保持原有目录结构，源码量不会太多，应该还好。