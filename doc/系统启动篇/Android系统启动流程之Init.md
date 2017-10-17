## 前言

我们都知道Android系统的第一个进程叫init进程，它是Linux系统的第一个用户进程，这个进程会孵化出许多重要的进程，其中一个就是Zygote进程，这个我会在下一篇文章中讲到。

init进程作为天字第一号进程，自然有它的特别之处，本文将详细讲解它的前世今生，主要讲两个方面：

- init进程由哪里来
- init进程做了些什么

本文涉及到的文件
```
msm/arch/arm64/kernel/head.S
msm/init/main.c
platform/core/init/init.cpp
```

## 一、init进程前世

很多文章都讲说init进程是Android的第一个进程，它的进程号是1，其实这种说法不准确，因为既然进程号是1，那么有没有进程号是0的进程呢，其实是有的。

这个进程名字叫idle，它才是系统的第一个进程，也是唯一一个没有通过fork或者kernel_thread产生的进程，它在完成初始化操作后，主要负责进程调度、交换。

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
	const struct sched_param param = { .sched_priority = 1 };

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	sched_setscheduler_nocheck(kthreadd_task, SCHED_FIFO, &param);
	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}
```




