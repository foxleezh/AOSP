## 前言
源码下载是我们分析源码的开始，Android源码可以全量下载，也可以单个下载，我们先介绍全量下载
## 全量下载
官方文档 https://source.android.com/source/downloading ，只要按照上面一步步做就可以了，但是由于需要翻墙，国内无法直接访问，而整个Android项目源码巨大，即便是翻墙后下载也很慢，所以还是使用国内镜像比较好。

我推荐清华大学开源镜像，地址 https://mirrors.tuna.tsinghua.edu.cn/help/AOSP/ ，这上面也是有完整的教程，我就不复制粘贴了，但是有一点要注意，你一定要备一个比较大的磁盘，至少60个G吧，还不算后期编译的。

我们分析源码其实是不需要全部代码的，因为AOSP不仅包括系统源码，还有些工具代码，如aapt，adb等，这些我们根本不需要，而且即便是系统源码，也不是所有我们都需要看，如果真的全部看，你这辈子都看不完，所以我还是推荐大家单个下载。

## 单个下载
官方地址 https://android.googlesource.com/ ，比如我们要下载platform/frameworks/base/目录下的代码，我们可以git clone https://android.googlesource.com/platform/frameworks/base ，不过这个还是会遇到翻墙的问题，当然我们也可以用镜像。

镜像地址 https://aosp.tuna.tsinghua.edu.cn/ ，比如我们要下载platform/frameworks/base/目录，就用git clone https://aosp.tuna.tsinghua.edu.cn/platform/frameworks/base ，如果你带宽够的话，一般几分钟就可以下载好你想要的单个源码了。

如果你想下载单个文件，或者搜索文件名及代码，可以访问 http://androidxref.com/ ，这里有部分Android的源码

## 目录结构

先上一张图，整个Android项目的架构图
![](http://upload-images.jianshu.io/upload_images/3387045-51f3f9371353714e.jpg?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

我们都知道Android系统从上到下大致分为这四层，所以我们以这四层为基础，讲解下AOSP的目录结构：

- 第一层：应用程序层(applications)对应根目录下[platform/packages/apps](https://android.googlesource.com/platform/packages/apps)
- 第二层：应用程序框架层(application framework)对应根目录下的[platform/frameworks](https://android.googlesource.com/platform/frameworks)
- 第三层：运行库层包括运行库(libraries)和android运行时环境(android runtime)
 - libraries对应目录很多，其中libc库对应的是[platform/bionic](https://android.googlesource.com/platform/bionic)
 - android运行时环境，Core Libraries 对应根目录下的[platform/libcore](https://android.googlesource.com/platform/libcore)，Dalvik Virtual Machine 对应根目录下的[platform/dalvik](https://android.googlesource.com/platform/dalvik) ，不过现在已经是ART了，所以目录是[platform/art](https://android.googlesource.com/platform/art)
- 第四层：Linux内核层对应根目录下的[kernel](https://android.googlesource.com/kernel),每一个目录对应了一个kernel的版本，因为Android要兼容各种芯片，下面罗列一下：
 - goldfish 项目包含适用于所模拟的平台的内核源代码。
 - msm 项目包含适用于 ADP1、ADP2、Nexus One、Nexus 4、Nexus 5、Nexus 6、Nexus 5X、Nexus 6P、Nexus 7 (2013)、Pixel 和 Pixel XL 的源代码，可用作使用 Qualcomm MSM 芯片组的起点。
 - omap 项目用于 PandaBoard 和 Galaxy Nexus，可用作使用 TI OMAP 芯片组的起点。
 - samsung 项目用于 Nexus S，可用作使用 Samsung Hummingbird 芯片组的起点。
 - tegra 项目用于 Xoom、Nexus 7 (2012)、Nexus 9，可用作使用 NVIDIA Tegra 芯片组的起点。
 - exynos 项目包含适用于 Nexus 10 的内核源代码，可用作使用 Samsung Exynos 芯片组的起点。
 - x86_64 项目包含适用于 Nexus Player 的内核源代码，可用作使用 Intel x86_64 芯片组的起点。
 - hikey-linaro 项目用于 HiKey 参考板，可用作使用 HiSilicon 620 芯片组的起点。
- 三、四层中间还有个硬件抽象层(HAL)对应根目录下的[platform/hardware](https://android.googlesource.com/platform/hardware)


目前我下载的目录如下：

git clone https://aosp.tuna.tsinghua.edu.cn/platform/packages/apps/Launcher2 <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/frameworks/base <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/frameworks/native <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/system/core <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/bionic <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/libcore <br>
git clone https://aosp.tuna.tsinghua.edu.cn/platform/art <br>
git clone https://aosp.tuna.tsinghua.edu.cn/kernel/msm <br>