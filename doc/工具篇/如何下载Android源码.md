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

如果你想看Linux Kernel的源码，可以访问 https://www.kernel.org/ ，不过我还是推荐在 https://android.googlesource.com/ 下载，对应目录是https://android.googlesource.com/kernel/msm ，也可以用镜像

另外AOSP中引入了很多C++标准库的代码，这个也需要去下载下，下载地址 https://www.gnu.org/software/libc/sources.html ，也可以直接 git clone git://sourceware.org/git/glibc.git