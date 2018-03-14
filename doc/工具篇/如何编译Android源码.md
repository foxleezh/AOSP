## 下载Android源码
由于Google被墙的原因，下载Android源码还是用国内镜像的好，我用的是[清华源](https://mirrors.tuna.tsinghua.edu.cn/help/AOSP/),按照上面的步骤一步步来

我选用的是直接下载一个离线包，然后再更新

- 下载离线包 [https://mirrors.tuna.tsinghua.edu.cn/aosp-monthly/aosp-latest.tar](https://mirrors.tuna.tsinghua.edu.cn/aosp-monthly/aosp-latest.tar)
- 解压 tar xf aosp-latest.tar
- 安装repo，sudo apt-get install repo
- 在～/.bashrc中加入如下变量，记得source
```
export REPO_URL='https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/'
export JACK_SERVER_VM_ARGUMENTS='-Dfile.encoding=UTF-8 -XX:+TieredCompilation -Xmx4g'
```
- 设置下git的最大缓存，如果设置过请无视
```
sudo git config --global http.postBuffer 524288000
```


- 进入解压后的目录aosp , 然后 repo sync 更新最新代码

我下载的时候，离线包有40多G，解压出来也有40多G，也就是你需要90G左右的空间，解压完后，你可以把离线包删除掉（实在太大了，我的固态只有128G），执行完repo sync后有70多G

默认同步下来的是master分支，如果要切换到指定的分支，可以先查看下有哪些分支
``` bash
cd .repo/manifests
git branch -a | cut -d / -f 3
```
然后返回到aosp目录，切换分支并同步
```
repo init -u https://aosp.tuna.tsinghua.edu.cn/platform/manifest -b android-8.0.0_r17
```
repo sync可能会出错，所以我们写个脚本down.sh不断重试
```bash
#!/bin/bash
repo sync  -j8
while [ $? = 1 ]; do
        echo “======sync failed, re-sync again======”
        sleep 3
        repo sync  -j8
done
```
执行down.sh
```shell
chmod a+x down.sh
sh down.sh
```

执行repo sync可能会报503的错，这个是服务器限制了最大并发，把脚本中repo  sync -j后的8改为4或者2就行，其实也可以无视

可能还会报无法连接到 gerrit.googlesource.com，这个设置下环境变量
```
export REPO_URL='https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/'
```

执行完repo sync后就会把.repo里的东西还原出来，就像.git一样，这时android的源码就呈现在你面前了。repo sync是比较耗时的，所以如果你不需要最新的代码，也可以在第一步不先repo sync同步离线包，而是先切换分支再同步，这样可以节省点时间

repo sync后的目录是这个样子
![](http://upload-images.jianshu.io/upload_images/3387045-25168d74087b06ee.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

## 构建编译环境
安装openjdk，记住这里是openjdk，与我们平常用的oracle的jdk不一样，如果你电脑里配置了java环境变量，请将它注释掉，重新安装openjdk，我最开始编译的时候就是没搞清楚，一直卡在这儿
```
sudo apt-get install openjdk-8-jdk
```
具体安装哪个版本的jdk，要根据你编译需求

| Android版本|	编译要求的JDK版本|
| :-- | :--|
| AOSP的Android主线	| OpenJDK 8|
| Android 5.x至android 6.0	| OpenJDK 7|
| Android 2.3.x至Android 4.4.x| 	Oracle JDK 6|
| Android 1.5至Android 2.2.x| 	Oracle JDK 5|

安装完open-jdk后，输入如下命令切换下java版本
```
sudo update-alternatives --config java
sudo update-alternatives --config javac
```

安装需要的软件，如下是Ubuntu 14.04及以上需要安装的，其他Ubuntu参考[Google官网](https://source.android.com/source/initializing)
```
sudo apt-get install git-core gnupg flex bison gperf build-essential zip curl zlib1g-dev gcc-multilib g++-multilib libc6-dev-i386 lib32ncurses5-dev x11proto-core-dev libx11-dev lib32z-dev ccache libgl1-mesa-dev libxml2-utils xsltproc unzip lib64stdc++6:i386 mesa-utils
```
## 开始编译

在编译前先确定你是要刷到真机还是模拟器，如果要刷到真机，先要下载驱动，将驱动放到源码根目录下一起编译，可参考后文「刷到真机」

初始化编译环境
```
source build/envsetup.sh
```
选择lunch目标
```
lunch aosp_arm-eng
```
这个aosp_arm-eng由两部分组成，aosp_arm是编译针对的平台，比如要编译x86就是aosp_x86，如果你不知道选什么直接输入lunch，会有选择提示
![](http://upload-images.jianshu.io/upload_images/3387045-f1046a102ee3fe68.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

eng表示编译之后的标记



| 编译类型| 使用情况|
| :-- | :--|
| user	| 权限受限；适用于生产环境;比如各厂商的发行版|
| userdebug	| 与“user”类似，但具有 root 权限和可调试性；是进行调试时的首选编译类型|
| eng	| 具有额外调试工具的开发配置，在userdebug的基础有更多配置|

输出目录默认在out目录下，如果你想自定义输出目录，可以配置下环境变量，记得source
```
export OUT_DIR_COMMON_BASE="/androidsource/out"
```

最后就是执行编译了
```
make -j8
```
j后面的8表示执行的线程数，可以根据你的cpu来确定，比如我是4核心，双线程的cpu,所以4×2=8，其实也不用那么在意这个数字，一般8还是比较合适的

在编译前，最好确认下你的空间，至少准备个70G的空间，我这边编译完占了69.7G，然后就是选择一个空闲时间，比如晚上，我编译花了4小时左右，来张编译成功的图
![](http://upload-images.jianshu.io/upload_images/3387045-3fb3609eb3b3d90a.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

![](http://upload-images.jianshu.io/upload_images/3387045-b370106a9e9800f4.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

## 在模拟器运行
直接运行 emulator 即可，如果你编译完成后关闭了电脑，可能会找不到emulator，这时可以重新执行下之前编译的命令
```
source build/envsetup.sh
lunch aosp_arm-eng
```

运行emulator 可能会遇到报错：glxinfo: not found
```
sudo apt-get install mesa-utils
```

报错 unable to load driver: i965_dri.so
```
mv 源码目录/prebuilts/android-emulator/linux-x86_64/lib64/libstdc++/libstdc++.so.6{,.bak}
mv 源码目录/prebuilts/android-emulator/linux-x86_64/lib64/libstdc++/libstdc++.so.6.0.18{,.bak}
ln -s /usr/lib32/libstdc++.so.6  源码目录/prebuilts/android-emulator/linux-x86_64/lib64/libstdc++/
```
前两句是删除并备份原有的两个文件，第三句是创建一个链接

另外还有个解决方法是的emulator后加上-gpu off

## 刷到真机

国内的厂商都是定制的，要想将AOSP直接刷上去比较难，因为拿不到驱动，而Google的手机驱动是开源的，所以还是买个Google亲儿子吧，这里我推荐 Nexus 6P ,我在某宝上买的二手，几百块钱，开发足够了

首先要找到对应的版本和型号
https://source.android.com/source/build-numbers#source-code-tags-and-builds

官方文档上写得很清楚了，我用的是
```
OPR5.170623.007	android-8.0.0_r17	Oreo	Nexus 6P
```

记住这个OPR5.170623.007，这个是版本细分标记，可以用它来找到对应驱动

驱动下载地址 https://developers.google.com/android/drivers#anglernmf26f

下载下来是个压缩文件，里面有一个sh文件，直接执行
```shell
sh extract-huawei-angler.sh
```
它会先让你看一长串协议，按住Enter键不放就可以往下看，看到最后会让你输入 I ACCEPT，然后就会将里面的文件解压出来，这里有点坑就是如果你一直按Enter可能会跳过I ACCEPT这步，然后又得重新来，所以快要到协议结束时要手动慢慢按Enter

解压出来有这些文件
```
vendor/
vendor/huawei/
vendor/huawei/angler/
vendor/huawei/angler/android-info.txt
vendor/huawei/angler/BoardConfigVendor.mk
vendor/huawei/angler/BoardConfigPartial.mk
vendor/huawei/angler/proprietary/
vendor/huawei/angler/proprietary/vendor.img
vendor/huawei/angler/device-partial.mk
vendor/huawei/angler/device-vendor.mk
```

把vendor目录直接拷贝到源码根目录下进行重新编译,angler表示nexus 6p
```
. build/envsetup.sh
lunch aosp_angler-userdebug
make -j4
```

接下来的操作参考[Google官网](https://source.android.com/setup/running)

编译完成后，usb连接上手机，首先进入手机将开发者模式打开，里面有个OEM 解锁，将这个锁打开
也可以用命令 fastboot flashing unlock

进入刷机模式，nexus 6p是 按住音量调低键，然后按住电源键。

也可以用命令 adb reboot bootloader ，进入刷机模式后手机会显示一个打开盖子的android机器人

一切准备就绪，执行刷机 fastboot flashall -w ，两分钟左右就刷机成功了
![](http://upload-images.jianshu.io/upload_images/3387045-bf16b9ea452f471d.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)


