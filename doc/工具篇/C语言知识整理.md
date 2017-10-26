为了方便大家理解C/C++的语法，我将源码中涉及到的一些小知识整理一下，以源码分析的顺序列出，我会在知识点下列出出现的地方，大家也可以对照着看。

### 1.oninline、inline、__init、void
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言oninline与inline是一对意义相反的关键字，inline的作用是编译期间直接替换代码块，也就是说编译后就没有这个方法了，而是直接把代码块替换调用这个函数的地方，oninline就相反，强制不替换，保持原有的函数
 * __init_refok是__init的扩展，__init 定义的初始化函数会放入名叫.init.text的输入段，当内核启动完毕后，这个段中的内存会被释放掉，在本文中有讲，关注3.5 free_initmem。
 * 不带参数的方法会加一个void参数
 */
static noinline void __init_refok rest_init(void)
{
 }
```
更多使用参考[GCC特性之__init修饰解析](http://blog.csdn.net/kasalyn/article/details/17012099)
### 2.struct
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言中const相当于Java中的final static， 表示常量
 * struct是结构体，相当于Java中定义了一个实体类，里面只有一些成员变量，{.sched_priority =1 }相当于new，然后将成员变量sched_priority的值赋为1
 */
const struct sched_param param = { .sched_priority = 1 }; 
```
### 3.函数指针
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言中支持方法传参，kernel_thread是函数，kernel_init也是函数，但是kernel_init却作为参数传递了过去，其实传递过去的是一个函数指针
 */
kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
```
更多使用参考[函数指针](http://www.cnblogs.com/haore147/p/3647262.html)

### 4. &用法
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言中&的作用是获得变量的内存地址
 */
kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
```
更多使用参考[C指针](http://www.runoob.com/cprogramming/c-pointers.html)

### 5. 函数指针定义，* 指针，unsigned
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言中 int (*fn)(void *)表示函数指针的定义，int是返回值，void是函数的参数，fn是名字
 * C语言中 * 表示指针，这个用法很多
 * unsigned表示无符号，一般与long,int,char等结合使用，表示范围只有正数，比如init表示范围-2147483648～2147483647 ，那unsigned表示范围0～4294967295，足足多了一倍
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
return do_fork(flags|CLONE_VM|CLONE_UNTRACED, (unsigned long)fn,
(unsigned long)arg, NULL, NULL);
}
```
更多使用参考 [深入解析C语言中函数指针](http://www.jb51.net/article/82738.htm) ，[C语言入门之指针用法教程](http://www.jb51.net/article/82738.htm)

### 6. ->
出现在《Android系统启动流程之Linux内核》
```C
/*
 * C语言中 -> 用于指向结构体 struct 中的数据
 */
if (pnr->nr == nr && pnr->ns == ns)
```
更多使用参考 [C语言中 -> 是什么意思](http://blog.csdn.net/littesss/article/details/71185916)

### 6. #ifdef、#else、#endif(条件编译)
出现在《Android系统启动流程之Linux内核》
```C
	 /*
	  * C语言中#ifdef和#else、#endif是条件编译语句，也就是说在满足某些条件的时候，夹在这几个关键字中间的代码才编译，不满足就不编译
	  * 下面这句话的意思就是如果定义了CONFIG_X86这个宏，就把boot_init_stack_canary这个代码编译进去
	  */
#ifdef CONFIG_X86
	/*
	 * If we're the non-boot CPU, nothing set the stack canary up
	 * for us. The boot CPU already has it initialized but no harm
	 * in doing it again. This is a good place for updating it, as
	 * we wont ever return from this function (so the invalid
	 * canaries already on the stack wont ever trigger).
	 */
	boot_init_stack_canary();//只有在x86这种non-boot CPU机器上执行，该函数主要用于初始化stack_canary的值,用于防止栈溢出
#endif
```
更多使用参考 [条件编译#ifdef的妙用详解_透彻](http://www.cnblogs.com/wengzilin/archive/2012/04/26/2471018.html)