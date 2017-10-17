## 前言

我们都知道Android系统的第一个进程叫init进程，它是Linux系统的第一个用户进程，这个进程会孵化出许多重要的进程，其中一个就是Zygote进程，这个我会在下一篇文章中讲到。

init进程作为天字第一号进程，自然有它的特别之处，本文将详细讲解它的前世今生，主要讲两个方面：

- init进程由哪里来
- init进程做了些什么

本文涉及到的文件
```
msm/arch/arm64/kernel/head.S
msm/init/main.c
msm/kernel/cpu/idle.c
platform/core/init/init.cpp
```

## 一、init进程前世

很多文章都讲说init进程是Android的第一个进程，它的进程号是1，其实这种说法不准确，因为既然进程号是1，那么有没有进程号是0的进程呢，其实是有的。

这个进程名字叫init_task，后期会退化为idle，它才是系统的第一个进程，也是唯一一个没有通过fork或者kernel_thread产生的进程，它在完成初始化操作后，主要负责进程调度、交换。

idle进程的启动是用汇编语言写的，对应文件是msm/arch/arm64/kernel/head.S，因为都是用汇编语言写的，我就不多介绍了，具体可参考 [kernel 启动流程之head.S](http://blog.csdn.net/forever_2015/article/details/52885250) ,其中有一句比较重要


```c
340 	str	x22, [x4]			// Save processor ID
341 	str	x21, [x5]			// Save FDT pointer
342 	str	x24, [x6]			// Save PHYS_OFFSET
343 	mov	x29, #0
344 	b	start_kernel        //跳转start_kernel方法
```

b 就是跳转的意思，跳转到start_kernel.h，这个头文件对应的实现在msm/init/main.c，start_kernel方法在最后会调用rest_init方法，这个方法开启了init进程，我们来分析下rest_init方法

```C
static noinline void __init_refok rest_init(void)
{
	int pid;
	const struct sched_param param = { .sched_priority = 1 }; //初始化优先级为1的进程调度策略，取值1~99，1为最小

	rcu_scheduler_starting(); //启动RCU机制，这个与后面的rcu_read_lock和rcu_read_unlock是配套的，用于多核同步
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND); //用kernel_thread方式创建init进程，CLONE_FS 子进程与父进程共享相同的文件系统，包括root、当前目录、umask，CLONE_SIGHAND  子进程与父进程共享相同的信号处理（signal handler）表
	numa_default_policy(); // 设定NUMA系统的默认内存访问策略
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);//用kernel_thread方式创建kthreadd进程，CLONE_FILES  子进程与父进程共享相同的文件描述符（file descriptor）表
	rcu_read_lock(); //打开RCU读取锁，在此期间无法进行进程切换
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);// 获取kthreadd的进程描述符，期间需要检索进程pid的使用链表，所以要加锁
	rcu_read_unlock(); //关闭RCU读取锁
	sched_setscheduler_nocheck(kthreadd_task, SCHED_FIFO, &param); //设置kthreadd的进程调度策略，SCHED_FIFO 实时调度策略，即马上调用，先到先服务，param的优先级之前定义为1
	complete(&kthreadd_done); // complete和wait_for_completion是配套的同步机制，跟java的notify和wait差不多，之前kernel_init方法调用了wait_for_completion(&kthreadd_done)，这里调用complete就是通知kernel_init进程kthreadd进程已创建完成，可以继续执行

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);//当前0号进程init_task设置为idle进程
	schedule_preempt_disabled(); //0号进程主动请求调度，让出cpu，1号进程kernel_init将会运行,并且禁止抢占
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);// 这个方法会调用cpu_idle_loop()使得idle进程进入自己的事件处理循环
}
```
http://blog.csdn.net/gatieme/article/details/51484562
http://blog.csdn.net/xichangbao/article/details/52938240




