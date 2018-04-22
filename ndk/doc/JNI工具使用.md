## 生成头文件
在src/main/java目录下输入命令,-d表示输出目录，com.foxleezh.ndk.cpp.NativeTest表示完整路径
```shell
javah -d ../cpp com.foxleezh.ndk.cpp.NativeTest
```

## 查看Java类的签名信息
找到java类对应的.class文件，在目录build/intermediates/classes/debug下运行如下命令：
```shell
javap -s com.foxleezh.ndk.JavaTest
```