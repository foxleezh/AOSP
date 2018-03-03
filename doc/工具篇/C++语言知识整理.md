为了方便大家理解C/C++的语法，我将源码中涉及到的一些小知识整理一下，以源码分析的顺序列出，我会在知识点下列出出现的地方，大家也可以对照着看。

### 1.String相关函数

#### 1.1 strcmp
比较两个字符串,设这两个字符串为str1，str2<br>
若str1==str2，则返回零<br>
若str1 < str2，则返回负数<br>
若str1 > str2，则返回正数<br>

#### 1.2 clear
清空字符串
#### 1.3 reserve
函数reserve()将字符串的容量设置为至少size. 如果size指定的数值要小于当前字符串中的字符数, 容量将被设置为可以恰好容纳字符的数值.
#### 1.4 strcspn
strcspn用于返回字符所在下标,相当于String的indexof
```
size_t entry_key_len = strcspn(ENV[n], "=");
```

### 2.文件读写
#### 2.1 access
判断文件是否存在，并判断文件读写权限
第一个参数是文件路径，第二个是读写权限，如果返回-1表示出错

#### 2.2 open(const char* pathname, int flags, ...) 

打开文件,第一个参数是文件路径，第二个是模式，常见的有

| 参数 | 含义 |
| :-- | :-- |
| O_RDONLY    |   只读模式 |
| O_WRONLY     |   只写模式 |
| O_CREAT      |   如果文件不存在就创建一个 |
| O_CLOEXEC     |  即当调用exec（）函数成功后，文件描述符会自动关闭,且为原子操作。 |
| O_BINARY     |   以二进制方式打开 |

### 其他

#### 1.模板

模板函数和模板类的定义和使用。用户在程序编译前，只定义了模板函数

```c
template <typename T, typename T2>

void func(T1 t1, T2 t2){}
```



以及模板类
```C
template <typename T, typename T2>

class MyTemplateClass

{

。。。

}；

```

我们可以把模板看作Java中的泛型