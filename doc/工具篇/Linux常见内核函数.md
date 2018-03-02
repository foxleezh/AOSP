Linux系统有提供许多方便的API，就像Andoird中TextView的setText方法一样，我们只需要简单调用就可以实现一些功能，为了方便大家阅读Linux源码，我将一些常用的API列举出来

我先大致分个类吧

- 进程与进程调度
- 同步与锁
- 内存与内存策略

## 一、进程与进程调度
### 1.1 kernel_thread
出现在《Android系统启动流程之Linux内核》
```C
kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
```
这个函数作用是启动进程
- 第一个参数表示新进程工作函数，相当于Java的构造函数
- 第二个参数是工作函数的参数，相当于Java带参构造函数的参数
- 第三个参数表示启动方式

|参数名|作用|
| :-- | :-- |
| CLONE_PARENT | 创建的子进程的父进程是调用者的父进程，新进程与创建它的进程成了“兄弟”而不是“父子”|
| CLONE_FS    |      子进程与父进程共享相同的文件系统，包括root、当前目录、umask |
| CLONE_FILES   |  子进程与父进程共享相同的文件描述符（file descriptor）表 |
| CLONE_NEWNS | 在新的namespace启动子进程，namespace描述了进程的文件hierarchy |
| CLONE_SIGHAND | 子进程与父进程共享相同的信号处理（signal handler）表 |
| CLONE_PTRACE | 若父进程被trace，子进程也被trace |
| CLONE_UNTRACED | 若父进程被trace，子进程不被trace |
| CLONE_VFORK  |  父进程被挂起，直至子进程释放虚拟内存资源 |
| CLONE_VM     |     子进程与父进程运行于相同的内存空间 |
| CLONE_PID    |    子进程在创建时PID与父进程一致 |
| CLONE_THREAD  | Linux 2.4中增加以支持POSIX线程标准，子进程与父进程共享相同的线程群 |
### 1.2 sched_setscheduler_nocheck
出现在《Android系统启动流程之Linux内核》
```C
sched_setscheduler_nocheck(kthreadd_task, SCHED_FIFO, &param);
```
这个函数作用是设置进程调度策略
- 第一个参数是进程的task_struct
- 第二个是进程调度策略
- 第三个是进程优先级

进程调度策略如下:
- SCHED_FIFO和SCHED_RR和SCHED_DEADLINE则采用不同的调度策略调度实时进程，优先级最高

- SCHED_NORMAL和SCHED_BATCH调度普通的非实时进程，优先级普通

- SCHED_IDLE则在系统空闲时调用idle进程，优先级最低

参考[Linux进程调度器的设计](http://blog.csdn.net/gatieme/article/details/51702662)
## 二、同步与锁
### 2.1 rcu_read_lock、rcu_read_unlock
出现在《Android系统启动流程之Linux内核》
```C
	rcu_read_lock(); 
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
```
RCU（Read-Copy Update）是数据同步的一种方式，在当前的Linux内核中发挥着重要的作用。RCU主要针对的数据对象是链表，目的是提高遍历读取数据的效率，为了达到目的使用RCU机制读取数据的时候不对链表进行耗时的加锁操作。这样在同一时间可以有多个线程同时读取该链表，并且允许一个线程对链表进行修改（修改的时候，需要加锁）

参考[Linux 2.6内核中新的锁机制--RCU](https://www.ibm.com/developerworks/cn/linux/l-rcu/)

## 三、内存与内存策略
### 3.1 numa_default_policy
设定NUMA系统的默认内存访问策略

### 3.2 mmap
将一个文件或者其它对象映射进内存
```C
map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0); 
```
start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址。

length：映射区的长度。长度单位是 以字节为单位，不足一内存页按一内存页处理

prot：期望的内存保护标志，不能与文件的打开模式冲突。是以下的某个值，可以通过or运算合理地组合在一起

| 参数 | 含义 |
| :-- | :-- |
| PROT_EXEC | 页内容可以被执行| 
| PROT_READ | 页内容可以被读取| 
| PROT_WRITE | 页可以被写入| 
| PROT_NONE | 页不可访问| 

flags：指定映射对象的类型，映射选项和映射页是否可以共享。它的值可以是一个或者多个以下位的组合体

| 参数 | 含义 |
| :-- | :-- |
| MAP_FIXED |使用指定的映射起始地址，如果由start和len参数指定的内存区重叠于现存的映射空间，重叠部分将会被丢弃。如果指定的起始地址不可用，操作将会失败。并且起始地址必须落在页的边界上|
| MAP_SHARED |与其它所有映射这个对象的进程共享映射空间。对共享区的写入，相当于输出到文件。直到msync()或者munmap()被调用，文件实际上不会被更新|
| MAP_PRIVATE |建立一个写入时拷贝的私有映射。内存区域的写入不会影响到原文件。这个标志和以上标志是互斥的，只能使用其中一个|
| MAP_DENYWRITE |这个标志被忽略|
| MAP_EXECUTABLE |同上|
| MAP_NORESERVE |不要为这个映射保留交换空间。当交换空间被保留，对映射区修改的可能会得到保证。当交换空间不被保留，同时内存不足，对映射区的修改会引起段违例信号|
| MAP_LOCKED |锁定映射区的页面，从而防止页面被交换出内存|
| MAP_GROWSDOWN |用于堆栈，告诉内核VM系统，映射区可以向下扩展|
| MAP_ANONYMOUS |匿名映射，映射区不与任何文件关联|
| MAP_ANON |MAP_ANONYMOUS的别称，不再被使用|
| MAP_FILE |兼容标志，被忽略|
| MAP_32BIT |将映射区放在进程地址空间的低2GB，MAP_FIXED指定时会被忽略。当前这个标志只在x86-64平台上得到支持|
| MAP_POPULATE |为文件映射通过预读的方式准备好页表。随后对映射区的访问不会被页违例阻塞|
| MAP_NONBLOCK |仅和MAP_POPULATE一起使用时才有意义。不执行预读，只为已存在于内存中的页面建立页表入口|

fd：有效的文件描述词。一般是由open()函数返回，其值也可以设置为-1，此时需要指定flags参数中的MAP_ANON,表明进行的是匿名映射

off_toffset：被映射对象内容的起点。

## 四、通信
### 4.1 int socketpair(int d, int type, int protocol, int sv[2])
创建一对socket，用于本机内的进程通信<br>
参数分别是：<br>
- d 套接口的域 ,一般为AF_UNIX，表示Linux本机<br>
- type 套接口类型,参数比较多<br>
SOCK_STREAM或SOCK_DGRAM，即TCP或UDP<br>
SOCK_NONBLOCK   read不到数据不阻塞，直接返回0<br>
SOCK_CLOEXEC    设置文件描述符为O_CLOEXEC <br>
- protocol 使用的协议，值只能是0<br>
- sv 指向存储文件描述符的指针<br>

### 4.2  int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

- signum：要操作的信号。
- act：要设置的对信号的新处理方式。
- oldact：原来对信号的处理方式。
- 返回值：0 表示成功，-1 表示有错误发生。

 struct sigaction 类型用来描述对信号的处理，定义如下：
 struct sigaction
 {
  void     (*sa_handler)(int);
  void     (*sa_sigaction)(int, siginfo_t *, void *);
  sigset_t  sa_mask;
  int       sa_flags;
  void     (*sa_restorer)(void);
 };


sa_handler 是一个函数指针，其含义与 signal 函数中的信号处理函数类似
sa_sigaction 则是另一个信号处理函数，它有三个参数，可以获得关于信号的更详细的信息。当 sa_flags 成员的值包含了 SA_SIGINFO 标志时，系统将使用 sa_sigaction 函数作为信号处理函数，否则使用 sa_handler 作为信号处理函数。在某些系统中，成员 sa_handler 与 sa_sigaction 被放在联合体中，因此使用时不要同时设置。
sa_mask 成员用来指定在信号处理函数执行期间需要被屏蔽的信号，特别是当某个信号被处理时，它自身会被自动放入进程的信号掩码，因此在信号处理函数执行期间这个信号不会再度发生。
sa_flags 成员用于指定信号处理的行为，它可以是一下值的“按位或”组合。

|参数名|作用|
| :-- | :-- |
| SA_RESTART| 使被信号打断的系统调用自动重新发起|
| SA_NOCLDSTOP| 使父进程在它的子进程暂停或继续运行时不会收到 SIGCHLD 信号|
| SA_NOCLDWAIT| 使父进程在它的子进程退出时不会收到 SIGCHLD 信号，这时子进程如果退出也不会成为僵尸进程|
| SA_NODEFER| 使对信号的屏蔽无效，即在信号处理函数执行期间仍能发出这个信号|
| SA_RESETHAND| 信号处理之后重新设置为默认的处理方式|
| SA_SIGINFO| 使用 sa_sigaction 成员而不是 sa_handler 作为信号处理函数|

## 五、密钥

### 5.1 获取密钥信息
keyctl(KEYCTL_GET_KEYRING_ID, KEY_SPEC_SESSION_KEYRING, 1)

- KEYCTL_GET_KEYRING_ID 表示通过第二个参数的类型获取当前进程的密钥信息
- KEY_SPEC_SESSION_KEYRING 表示获取当前进程的SESSION_KEYRING（会话密钥环）
- 1 表示如果获取不到就新建一个

参考[linux手册](http://www.man7.org/linux/man-pages/man2/keyctl.2.html)