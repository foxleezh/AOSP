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