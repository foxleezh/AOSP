## 前言

Android本质上就是一个基于Linux内核的操作系统，与Ubuntu Linux、Fedora Linux类似，我们要讲Android，必定先要了解一些Linux内核的知识。

Linux内核的东西特别多，我也不可能全部讲完，由于本文主要讲解Android系统启动流程，所以这里主要讲一些内核启动相关的知识。

Linux内核启动主要涉及3个特殊的进程，idle进程(PID = 0), init进程(PID = 1)和kthreadd进程(PID = 2)，这三个进程是内核的基础。

- idle进程是Linux系统第一个进程，是init进程和kthreadd进程的父进程
- init进程是Linux系统第一个用户进程，是Android系统应用程序的始祖，我们的app都是直接或间接以它为父进程
- kthreadd进程是Linux系统内核管家，所有的内核线程都是直接或间接以它为父进程

![](http://upload-images.jianshu.io/upload_images/3387045-a5b38ee85b13c64d.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

本文将以这三个进程为线索，主要讲解以下内容：

- idle进程启动
- kthreadd进程启动
- init进程启动

本文涉及到的文件
```
msm/arch/arm64/kernel/head.S
msm/init/main.c
msm/kernel/rcutree.c
msm/kernel/fork.c
msm/mm/mempolicy.c
msm/kernel/kthread.c
msm/include/linux/kthread.h
msm/include/linux/rcupdate.h
msm/kernel/rcupdate.c
msm/kernel/pid.c
msm/include/linux/sched.h
msm/kernel/sched/core.c
msm/kernel/cpu/idle.c
msm/drivers/base/init.c
```

## 一、idle进程启动

很多文章讲Android都从init进程讲起，它的进程号是1，既然进程号是1，那么有没有进程号是0的进程呢，其实是有的。

这个进程名字叫init_task，后期会退化为idle，它是Linux系统的第一个进程(init进程是第一个用户进程)，也是唯一一个没有通过fork或者kernel_thread产生的进程，它在完成初始化操作后，主要负责进程调度、交换。

idle进程的启动是用汇编语言写的，对应文件是msm/arch/arm64/kernel/head.S，因为都是用汇编语言写的，我就不多介绍了，具体可参考 [kernel 启动流程之head.S](http://blog.csdn.net/forever_2015/article/details/52885250) ,这里面有一句比较重要


```c
340 	str	x22, [x4]			// Save processor ID
341 	str	x21, [x5]			// Save FDT pointer
342 	str	x24, [x6]			// Save PHYS_OFFSET
343 	mov	x29, #0
344 	b	start_kernel        //跳转start_kernel函数
```

第344行，b	start_kernel，b 就是跳转的意思，跳转到start_kernel.h，这个头文件对应的实现在msm/init/main.c，start_kernel函数在最后会调用rest_init函数，这个函数开启了init进程和kthreadd进程，我们着重分析下rest_init函数。

在讲源码前，我先说明下我分析源码的写作风格：
- 一般我会在函数下面写明该函数所在的位置，比如定义在msm/init/main.c中，这样大家就可以去项目里找到源文件
- 我会把源码相应的英文注释也一并copy进来，这样方便英文好的人可以看到原作者的注释
- 我会尽可能将函数中每一行代码的作用注释下(一般以//的形式注释在代码结尾)，大家在看源码的同时就可以理解这段代码作用，这也是我花时间最多的,请大家务必认真看。我也想过在源码外部统一通过行号来解释，但是感觉这样需要大家一会儿看源码，一会儿看解释，上下来回看不方便，所以干脆写在一起了
- 在函数结尾我尽可能总结下这个函数做了些什么，以及这个函数涉及到的一些知识
- 对于重要的函数，我会将函数中每一个调用的子函数再单独拿出来讲解
- 考虑到大家都是开发Android的比较多，对C/C++不太了解，在注释中我也会讲一些C/C++的知识，方便大家理解，C语言注释我一般用/** */的形式注释在代码顶头
- 为了更好的阅读体验，希望大家可以下载一下Source Insight同步看代码，[使用教程](https://juejin.im/post/59ec35f8f265da4307026b79) ,可以直接将[项目](https://github.com/foxleezh/AOSP)中app/src/main/cpp作为目录加入到Source Insight中

### 1.1 rest_init
定义在msm/init/main.c中
```C
/*
 * C语言oninline与inline是一对意义相反的关键字，inline的作用是编译期间直接替换代码块，也就是说编译后就没有这个方法了，而是直接把代码块替换调用这个函数的地方，oninline就相反，强制不替换，保持原有的函数
 * __init_refok是__init的扩展，__init 定义的初始化函数会放入名叫.init.text的输入段，当内核启动完毕后，这个段中的内存会被释放掉，在本文中有讲，关注3.5 free_initmem。
 * 不带参数的方法会加一个void参数
 */
static noinline void __init_refok rest_init(void)
{
	int pid;
	/*
	 * C语言中const相当于Java中的final static， 表示常量
	 * struct是结构体，相当于Java中定义了一个实体类，里面只有一些成员变量，{.sched_priority =1 }相当于new，然后将成员变量sched_priority的值赋为1
	 */
	const struct sched_param param = { .sched_priority = 1 }; //初始化优先级为1的进程调度策略，取值1~99，1为最小

	rcu_scheduler_starting(); //启动RCU机制，这个与后面的rcu_read_lock和rcu_read_unlock是配套的，用于多核同步
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */

	/*
     * C语言中支持方法传参，kernel_thread是函数，kernel_init也是函数，但是kernel_init却作为参数传递了过去，其实传递过去的是一个函数指针,参考[函数指针](http://www.cnblogs.com/haore147/p/3647262.html)
     * CLONE_FS这种大写的一般就是常量了，跟Java差不多
     */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND); //用kernel_thread方式创建init进程，CLONE_FS 子进程与父进程共享相同的文件系统，包括root、当前目录、umask，CLONE_SIGHAND  子进程与父进程共享相同的信号处理（signal handler）表
	numa_default_policy(); // 设定NUMA系统的默认内存访问策略
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);//用kernel_thread方式创建kthreadd进程，CLONE_FILES  子进程与父进程共享相同的文件描述符（file descriptor）表
	rcu_read_lock(); //打开RCU读取锁，在此期间无法进行进程切换
	/*
	 * C语言中&的作用是获得变量的内存地址，参考[C指针](http://www.runoob.com/cprogramming/c-pointers.html)
	 */
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);// 获取kthreadd的进程描述符，期间需要检索进程pid的使用链表，所以要加锁
	rcu_read_unlock(); //关闭RCU读取锁
	sched_setscheduler_nocheck(kthreadd_task, SCHED_FIFO, &param); //设置kthreadd的进程调度策略，SCHED_FIFO 实时调度策略，即马上调用，先到先服务，param的优先级之前定义为1
	complete(&kthreadd_done); // complete和wait_for_completion是配套的同步机制，跟java的notify和wait差不多，之前kernel_init函数调用了wait_for_completion(&kthreadd_done)，这里调用complete就是通知kernel_init进程kthreadd进程已创建完成，可以继续执行

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);//current表示当前进程，当前0号进程init_task设置为idle进程
	schedule_preempt_disabled(); //0号进程主动请求调度，让出cpu，1号进程kernel_init将会运行,并且禁止抢占
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);// 这个函数会调用cpu_idle_loop()使得idle进程进入自己的事件处理循环
}
```

rest_init的字面意思是剩余的初始化，但是它却一点都不剩余，它创建了Linux系统中两个重要的进程init和kthreadd，并且将init_task进程变为idle进程，接下来我将把rest_init中的方法逐个解析，方便大家理解。

### 1.2 rcu_scheduler_starting
定义在msm/kernel/rcutree.c
```C
/*
 * This function is invoked towards the end of the scheduler's initialization
 * process.  Before this is called, the idle task might contain
 * RCU read-side critical sections (during which time, this idle
 * task is booting the system).  After this function is called, the
 * idle tasks are prohibited from containing RCU read-side critical
 * sections.  This function also enables RCU lockdep checking.
 */
void rcu_scheduler_starting(void)
{
	WARN_ON(num_online_cpus() != 1); //WARN_ON相当于警告，会打印出当前栈信息，不会重启， num_online_cpus表示当前启动的cpu数
	WARN_ON(nr_context_switches() > 0); // nr_context_switches 进行进程切换的次数
	rcu_scheduler_active = 1; //启用rcu机制
}
```

#### 1.3 kernel_thread
定义在msm/kernel/fork.c
```C
/*
 * Create a kernel thread.
 */
 
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
do_fork函数用于创建进程，它首先调用copy_process()创建新进程，然后调用wake_up_new_task()将进程放入运行队列中并启动新进程。
kernel_thread的第一个参数是一个函数引用，它相当于Java中的构造函数，会在创建进程后执行，第三个参数是创建进程的方式，具体如下：

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


### 1.4 kernel_init
定义在msm/init/main.c

这个函数比较重要，负责init进程的启动，我将放在第三节重点讲，这个函数首先调用kernel_init_freeable函数
```C
static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);

	...
}

```

wait_for_completion之前讲了，与complete是配套的同步机制，这里就是等待&kthreadd_done这个值complete，然后就可以继续执行

### 1.5 numa_default_policy
定义在msm/mm/mempolicy.c
```C
/* Reset policy of current process to default */
void numa_default_policy(void)
{
	do_set_mempolicy(MPOL_DEFAULT, 0, NULL); //设定NUMA系统的内存访问策略为MPOL_DEFAULT
}
```
### 1.6 kthreadd
定义在msm/kernel/kthread.c中

kthreadd进程我将在第二节中重点讲，它是内核中重要的进程，负责内核线程的调度和管理，内核线程基本都是以它为父进程的

### 1.7 rcu_read_lock & rcu_read_unlock
定义在msm/include/linux/rcupdate.h和msm/kernel/rcupdate.c中

RCU（Read-Copy Update）是数据同步的一种方式，在当前的Linux内核中发挥着重要的作用。RCU主要针对的数据对象是链表，目的是提高遍历读取数据的效率，为了达到目的使用RCU机制读取数据的时候不对链表进行耗时的加锁操作。这样在同一时间可以有多个线程同时读取该链表，并且允许一个线程对链表进行修改（修改的时候，需要加锁）
```C
static inline void rcu_read_lock(void)
{
	__rcu_read_lock();
	__acquire(RCU);
	rcu_lock_acquire(&rcu_lock_map);
	rcu_lockdep_assert(!rcu_is_cpu_idle(),
			   "rcu_read_lock() used illegally while idle");
}

static inline void rcu_read_unlock(void)
{
	rcu_lockdep_assert(!rcu_is_cpu_idle(),
			   "rcu_read_unlock() used illegally while idle");
	rcu_lock_release(&rcu_lock_map);
	__release(RCU);
	__rcu_read_unlock();
}
```

### 1.8 find_task_by_pid_ns
定义在msm/kernel/pid.c中

task_struct叫进程描述符，这个结构体包含了一个进程所需的所有信息，它定义在msm/include/linux/sched.h文件中。

它的结构十分复杂，本文就不重点讲了，可以参考[Linux进程描述符task_struct结构体详解](http://blog.csdn.net/gatieme/article/details/51383272)
```C
/*
 * Must be called under rcu_read_lock().
 */
struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns)
{
	rcu_lockdep_assert(rcu_read_lock_held(),
			   "find_task_by_pid_ns() needs rcu_read_lock()"
			   " protection"); //必须进行RCU加锁
	return pid_task(find_pid_ns(nr, ns), PIDTYPE_PID);
}

struct pid *find_pid_ns(int nr, struct pid_namespace *ns)
{
	struct upid *pnr;

	hlist_for_each_entry_rcu(pnr,
			&pid_hash[pid_hashfn(nr, ns)], pid_chain)
			/*
			 * C语言中 -> 用于指向结构体 struct 中的数据
			 */
		if (pnr->nr == nr && pnr->ns == ns)
			return container_of(pnr, struct pid,
					numbers[ns->level]); //遍历hash表，找到struct pid

	return NULL;
}

struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result = NULL;
	if (pid) {
		struct hlist_node *first;
		first = rcu_dereference_check(hlist_first_rcu(&pid->tasks[type]),
					      lockdep_tasklist_lock_is_held());
		if (first)
			result = hlist_entry(first, struct task_struct, pids[(type)].node); //从hash表中找出struct task_struct
	}
	return result;
}
```
find_task_by_pid_ns的作用就是根据pid，在hash表中获得对应pid的task_struct
### 1.9 sched_setscheduler_nocheck
定义在msm/kernel/sched/core.c中
```C
int sched_setscheduler_nocheck(struct task_struct *p, int policy,
			       const struct sched_param *param)
{
	struct sched_attr attr = {
		.sched_policy   = policy,
		.sched_priority = param->sched_priority
	};
	return __sched_setscheduler(p, &attr, false); //设置进程调度策略
}
```
linux内核目前实现了6种调度策略(即调度算法), 用于对不同类型的进程进行调度, 或者支持某些特殊的功能

- SCHED_FIFO和SCHED_RR和SCHED_DEADLINE则采用不同的调度策略调度实时进程，优先级最高

- SCHED_NORMAL和SCHED_BATCH调度普通的非实时进程，优先级普通

- SCHED_IDLE则在系统空闲时调用idle进程，优先级最低

### 1.10 init_idle_bootup_task
定义在msm/kernel/sched/core.c中
```C
void __cpuinit init_idle_bootup_task(struct task_struct *idle)
{
	idle->sched_class = &idle_sched_class; //设置进程的调度器类为idle_sched_class
}
```
Linux依据其调度策略的不同实现了5个调度器类, 一个调度器类可以用一种种或者多种调度策略调度某一类进程, 也可以用于特殊情况或者调度特殊功能的进程.

其所属进程的优先级顺序为
```C
stop_sched_class -> dl_sched_class -> rt_sched_class -> fair_sched_class -> idle_sched_class
```
可见idle_sched_class的优先级最低，只有系统空闲时才调用idle进程

### 1.11 schedule_preempt_disabled
定义在msm/kernel/sched/core.c中
```C
/**
 * schedule_preempt_disabled - called with preemption disabled
 *
 * Returns with preemption disabled. Note: preempt_count must be 1
 */
void __sched schedule_preempt_disabled(void)
{
	sched_preempt_enable_no_resched(); //开启内核抢占
	schedule();  // 并主动请求调度，让出cpu
	preempt_disable(); // 关闭内核抢占
}
```

1.9到1.11都涉及到Linux的进程调度问题，可以参考 [Linux用户抢占和内核抢占详解](http://blog.csdn.net/gatieme/article/details/51872618)

### 1.12 cpu_startup_entry
定义在msm/kernel/cpu/idle.c中
```C
void cpu_startup_entry(enum cpuhp_state state)
{
	/*
	 * This #ifdef needs to die, but it's too late in the cycle to
	 * make this generic (arm and sh have never invoked the canary
	 * init for the non boot cpus!). Will be fixed in 3.11
	 */
	 
	 
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
	__current_set_polling(); //设置本架构下面有标示轮询poll的bit位，保证cpu进行重新调度。
	arch_cpu_idle_prepare(); //进行idle前的准备工作，ARM64中没有实现
	per_cpu(idle_force_poll, smp_processor_id()) = 0;
	cpu_idle_loop(); //进入idle进程的事件循环
}

```
### 1.13 cpu_idle_loop
定义在msm/kernel/cpu/idle.c中
```C
/*
 * Generic idle loop implementation
 */
static void cpu_idle_loop(void)
{
	while (1) { //开启无限循环，进行进程调度
		tick_nohz_idle_enter(); //停止周期时钟

		while (!need_resched()) { //判断是否有设置TIF_NEED_RESCHED，只有系统没有进程需要调度时才执行while里面操作
			check_pgt_cache();
			rmb();

			local_irq_disable(); //关闭irq中断
			arch_cpu_idle_enter();

			/*
			 * In poll mode we reenable interrupts and spin.
			 *
			 * Also if we detected in the wakeup from idle
			 * path that the tick broadcast device expired
			 * for us, we don't want to go deep idle as we
			 * know that the IPI is going to arrive right
			 * away
			 */
			if (cpu_idle_force_poll ||
			    tick_check_broadcast_expired() ||
			    __get_cpu_var(idle_force_poll)) {
				cpu_idle_poll(); //进入 CPU 的poll mode模式，避免进入深度睡眠，可以处理 处理器间中断
			} else {
				if (!current_clr_polling_and_test()) {
					stop_critical_timings();
					rcu_idle_enter();
					arch_cpu_idle(); //进入 CPU 的 idle 模式，省电
					WARN_ON_ONCE(irqs_disabled());
					rcu_idle_exit();
					start_critical_timings();
				} else {
					local_irq_enable();
				}
				__current_set_polling();
			}
			arch_cpu_idle_exit();
		}
		tick_nohz_idle_exit(); //如果有进程需要调度，则先开启周期时钟
		schedule_preempt_disabled(); //让出cpu，执行调度
		if (cpu_is_offline(smp_processor_id())) //如果当前cpu处理offline状态，关闭idle进程
			arch_cpu_idle_dead();

	}
}
```

idle进程并不执行什么复杂的工作，只有在系统没有其他进程调度的时候才进入idle进程，而在idle进程中尽可能让cpu空闲下来，连周期时钟也关掉了，达到省电目的。当有其他进程需要调度的时候，马上开启周期时钟，然后让出cpu。

**小结**

idle进程是Linux系统的第一个进程，进程号是0，在完成系统环境初始化工作之后，开启了两个重要的进程，init进程和kthreadd进程，执行完创建工作之后，开启一个无限循环，负责进程的调度。

## 二、kthreadd进程启动

之前在rest_init函数中启动了kthreadd进程
```C
pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
```
进程创建成功后会执行kthreadd函数

### 2.1 kthreadd
定义在msm/kernel/kthread.c中
```C
int kthreadd(void *unused)
{
	struct task_struct *tsk = current;

	/* Setup a clean context for our children to inherit. */
	set_task_comm(tsk, "kthreadd");
	ignore_signals(tsk);
	set_cpus_allowed_ptr(tsk, cpu_all_mask); //  允许kthreadd在任意CPU上运行
	set_mems_allowed(node_states[N_MEMORY]);

	current->flags |= PF_NOFREEZE;

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE); //首先将线程状态设置为 TASK_INTERRUPTIBLE, 如果当前没有要创建的线程则主动放弃 CPU 完成调度.此进程变为阻塞态
		if (list_empty(&kthread_create_list)) //  没有需要创建的内核线程
			schedule(); //   执行一次调度, 让出CPU
		__set_current_state(TASK_RUNNING);//  运行到此表示 kthreadd 线程被唤醒(就是我们当前),设置进程运行状态为 TASK_RUNNING
		spin_lock(&kthread_create_lock); //spin_lock和spin_unlock是配套的加锁机制，spin_lock是加锁
		while (!list_empty(&kthread_create_list)) {
			struct kthread_create_info *create;

			create = list_entry(kthread_create_list.next,
					    struct kthread_create_info, list); //kthread_create_list是一个链表，从链表中取出下一个要创建的kthread_create_info,即线程创建信息
			list_del_init(&create->list); //删除create中的list
			spin_unlock(&kthread_create_lock); //解锁

			create_kthread(create); //创建线程

			spin_lock(&kthread_create_lock); 
		}
		spin_unlock(&kthread_create_lock);
	}

	return 0;
}
```
kthreadd函数的作用就是循环地从kthread_create_list链表中取出要创建的线程，然后执行create_kthread函数，直到kthread_create_list为空，让出CPU,进入睡眠，我们来看下create_kthread函数
### 2.2 create_kthread
定义在msm/kernel/kthread.c中
```C
static void create_kthread(struct kthread_create_info *create)
{
	int pid;

#ifdef CONFIG_NUMA
	current->pref_node_fork = create->node;
#endif
	/* We want our own signal handler (we take no signals by default). */
	pid = kernel_thread(kthread, create, CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		create->result = ERR_PTR(pid);
		complete(&create->done);
	}
}
```
其实这里面就是调用kernel_thread函数创建进程，然后执行kthread函数，注意不要搞混了，之前那个函数叫kthreadd，接下来看看kthread函数

### 2.3 kthread
定义在msm/kernel/kthread.c中
```C
static int kthread(void *_create)
{
	/* Copy data: it's on kthread's stack */
	struct kthread_create_info *create = _create;  // create 就是之前kthreadd函数循环取出的 kthread_create_info
	int (*threadfn)(void *data) = create->threadfn; //新线程工作函数
	void *data = create->data;
	struct kthread self;
	int ret;

	self.flags = 0;
	self.data = data;
	init_completion(&self.exited);
	init_completion(&self.parked);
	current->vfork_done = &self.exited;

	/* OK, tell user we're spawned, wait for stop or wakeup */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	create->result = current;
	complete(&create->done); //表示线程创建完毕
	schedule(); //让出CPU，注意这里并没有执行新线程的threadfn函数就直接进入睡眠了，然后等待线程被手动唤醒，然后才执行threadfn

	ret = -EINTR;

	if (!test_bit(KTHREAD_SHOULD_STOP, &self.flags)) {
		__kthread_parkme(&self);
		ret = threadfn(data);
	}
	/* we can't just return, we must preserve "self" on stack */
	do_exit(ret);
}
```

### 2.4 kthread_create & kthread_run
定义在msm/include/linux/kthread.h

kthreadd创建线程是遍历kthread_create_list列表，那kthread_create_list列表中的值是哪儿来的呢？我们知道Linux创建内核线程有两种方式，kthread_create和kthread_run

```C
#define kthread_create(threadfn, data, namefmt, arg...) \
	kthread_create_on_node(threadfn, data, -1, namefmt, ##arg)

#define kthread_run(threadfn, data, namefmt, ...)			   \
({									   \
	struct task_struct *__k						   \
		= kthread_create(threadfn, data, namefmt, ## __VA_ARGS__); \
	if (!IS_ERR(__k))						   \
		wake_up_process(__k);	//手动唤醒新线程				   \
	__k;								   \
})
```
kthread_create和kthread_run并不是函数，而是宏，宏相当于Java中的final static定义，在编译时会替换对应代码，宏的参数没有类型定义，多行宏的定义会在行末尾加上\

这两个宏最终都是调用kthread_create_on_node函数，只是kthread_run在线程创建完成后会手动唤醒，我们来看看kthread_create_on_node函数

### 2.5 kthread_create_on_node
定义在msm/kernel/kthread.c中

```C
/**
 * kthread_create_on_node - create a kthread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @node: memory node number.
 * @namefmt: printf-style name for the thread.
 *
 * Description: This helper function creates and names a kernel
 * thread.  The thread will be stopped: use wake_up_process() to start
 * it.  See also kthread_run().
 *
 * If thread is going to be bound on a particular cpu, give its node
 * in @node, to get NUMA affinity for kthread stack, or else give -1.
 * When woken, the thread will run @threadfn() with @data as its
 * argument. @threadfn() can either call do_exit() directly if it is a
 * standalone thread for which no one will call kthread_stop(), or
 * return when 'kthread_should_stop()' is true (which means
 * kthread_stop() has been called).  The return value should be zero
 * or a negative error number; it will be passed to kthread_stop().
 *
 * Returns a task_struct or ERR_PTR(-ENOMEM).
 */
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	struct kthread_create_info create;

	create.threadfn = threadfn;
	create.data = data;
	create.node = node;
	init_completion(&create.done);  //初始化&create.done，之前讲过completion和wait_for_completion同步

	spin_lock(&kthread_create_lock);  //加锁，之前也讲过
	list_add_tail(&create.list, &kthread_create_list);  //将要创建的线程加到kthread_create_list链表尾部
	spin_unlock(&kthread_create_lock);

	wake_up_process(kthreadd_task);  //唤醒kthreadd进程，开启列表循环创建线程
	wait_for_completion(&create.done);  //当&create.done complete时，会继续往下执行

	if (!IS_ERR(create.result)) {
		static const struct sched_param param = { .sched_priority = 0 };
		va_list args;  //不定参数定义，相当于Java中的... ，定义多个数量不定的参数

		va_start(args, namefmt);
		vsnprintf(create.result->comm, sizeof(create.result->comm),
			  namefmt, args);
		va_end(args);
		/*
		 * root may have changed our (kthreadd's) priority or CPU mask.
		 * The kernel thread should not inherit these properties.
		 */
		sched_setscheduler_nocheck(create.result, SCHED_NORMAL, &param);  //create.result类型为task_struct，该函数作用是设置新线程调度策略，SCHED_NORMAL 普通调度策略，非实时，优先级低于实时调度策略SCHED_FIFO和SCHED_RR，param的优先级上面定义为0
		set_cpus_allowed_ptr(create.result, cpu_all_mask); //允许新线程在任意CPU上运行
	}
	return create.result;
}
```

kthread_create_on_node主要作用就是在kthread_create_list链表尾部加上要创建的线程，然后唤醒kthreadd进程进行具体创建工作


**小结** 

kthreadd进程由idle通过kernel_thread创建，并始终运行在内核空间, 负责所有内核线程的调度和管理，所有的内核线程都是直接或者间接的以kthreadd为父进程。

- kthreadd进程会执行一个kthreadd的函数，该函数的作用就是遍历kthread_create_list链表，从链表中取出需要创建的内核线程进行创建, 创建成功后会执行kthread函数。

- kthread函数完成一些初始赋值后就让出CPU，并没有执行新线程的工作函数，因此需要手工 wake up被唤醒后，新线程才执行自己的真正工作函数。

- 当我们调用kthread_create和kthread_run创建的内核线程会被加入到kthread_create_list链表，kthread_create不会手动wake up新线程，kthread_run会手动wake up新线程。

其实这就是一个典型的生产者消费者模式，kthread_create和kthread_run负责生产各种内核线程创建需求，kthreadd开启循环去消费各种内核线程创建需求。

## 三、init进程启动

init进程分为前后两部分，前一部分是在内核启动的，主要是完成创建和内核初始化工作，内容都是跟Linux内核相关的;后一部分是在用户空间启动的，主要完成Android系统的初始化工作。

我这里要讲的是前一部分，后一部分将在下一篇文章中讲述。

之前在rest_init函数中启动了init进程
```C
kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
```
在创建完init进程后，会调用kernel_init函数

### 3.1 kernel_init
定义在msm/init/main.c中
```C
/*
 * __ref 这个跟之前讲的__init作用一样
 */
static int __ref kernel_init(void *unused)
{
	kernel_init_freeable(); //进行init进程的一些初始化操作
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();// 等待所有异步调用执行完成,，在释放内存前，必须完成所有的异步 __init 代码
	free_initmem();// 释放所有init.* 段中的内存
	mark_rodata_ro(); //arm64空实现
	system_state = SYSTEM_RUNNING;// 设置系统状态为运行状态
	numa_default_policy(); // 设定NUMA系统的默认内存访问策略

	flush_delayed_fput(); // 释放所有延时的struct file结构体

	if (ramdisk_execute_command) { //ramdisk_execute_command的值为"/init"
		if (!run_init_process(ramdisk_execute_command)) //运行根目录下的init程序
			return 0;
		pr_err("Failed to execute %s\n", ramdisk_execute_command);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) { //execute_command的值如果有定义就去根目录下找对应的应用程序,然后启动
		if (!run_init_process(execute_command))
			return 0;
		pr_err("Failed to execute %s.  Attempting defaults...\n",
			execute_command);
	}
	if (!run_init_process("/sbin/init") || //如果ramdisk_execute_command和execute_command定义的应用程序都没有找到，就到根目录下找 /sbin/init，/etc/init，/bin/init,/bin/sh 这四个应用程序进行启动
	    !run_init_process("/etc/init") ||
	    !run_init_process("/bin/init") ||
	    !run_init_process("/bin/sh"))
		return 0;

	panic("No init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}
```

kernel_init主要工作是完成一些init的初始化操作，然后去系统根目录下依次找ramdisk_execute_command和execute_command设置的应用程序，如果这两个目录都找不到，就依次去根目录下找 /sbin/init，/etc/init，/bin/init,/bin/sh 这四个应用程序进行启动，只要这些应用程序有一个启动了，其他就不启动了

ramdisk_execute_command和execute_command的值是通过bootloader传递过来的参数设置的，ramdisk_execute_command通过"rdinit"参数赋值，execute_command通过"init"参数赋值

ramdisk_execute_command如果没有被赋值，kernel_init_freeable函数会赋一个初始值"/init"

### 3.2 kernel_init_freeable
定义在msm/init/main.c中
```C
static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done); //等待&kthreadd_done这个值complete,这个在rest_init方法中有写，在ktreadd进程启动完成后设置为complete

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;//设置bitmask, 使得init进程可以使用PM并且允许I/O阻塞操作

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);//init进程可以分配物理页面
	/*
	 * init can run on any cpu.
	 */
	set_cpus_allowed_ptr(current, cpu_all_mask); //init进程可以在任意cpu上执行

	cad_pid = task_pid(current); //设置到init进程的pid号给cad_pid，cad就是ctrl-alt-del，设置init进程来处理ctrl-alt-del信号

	smp_prepare_cpus(setup_max_cpus);//设置smp初始化时的最大CPU数量，然后将对应数量的CPU状态设置为present

	do_pre_smp_initcalls();//调用__initcall_start到__initcall0_start之间的initcall_t函数指针
	lockup_detector_init(); //开启watchdog_threads，watchdog主要用来监控、管理CPU的运行状态

	smp_init();//启动cpu0外的其他cpu核
	sched_init_smp(); //进程调度域初始化

	do_basic_setup();//初始化设备，驱动等，这个方法比较重要，将在下面单独讲

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0) // 打开/dev/console，文件号0，作为init进程标准输入
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);// 标准输入
	(void) sys_dup(0);// 标准输出
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)  //如果 ramdisk_execute_command 没有赋值，则赋值为"/init"，之前有讲到
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) { // 尝试进入ramdisk_execute_command指向的文件，如果失败则重新挂载根文件系统
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 */

	/* rootfs is available now, try loading default modules */
	load_default_modules(); // 加载I/O调度的电梯算法
}

```
kernel_init_freeable函数做了很多重要的事情

- 启动了smp，smp全称是Symmetrical Multi-Processing，即对称多处理，是指在一个计算机上汇集了一组处理器(多CPU),各CPU之间共享内存子系统以及总线结构。
- 初始化设备和驱动程序
- 打开标准输入和输出
- 初始化文件系统

### 3.3 do_basic_setup
定义在msm/init/main.c中
```C
/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();//针对SMP系统，初始化内核control group的cpuset子系统。
	usermodehelper_init();// 创建khelper单线程工作队列，用于协助新建和运行用户空间程序
	shmem_init();// 初始化共享内存
	driver_init();// 初始化设备驱动，比较重要下面单独讲
	init_irq_proc();//创建/proc/irq目录, 并初始化系统中所有中断对应的子目录
	do_ctors();// 执行内核的构造函数
	usermodehelper_enable();// 启用usermodehelper
	do_initcalls();//遍历initcall_levels数组，调用里面的initcall函数，这里主要是对设备、驱动、文件系统进行初始化，之所有将函数封装到数组进行遍历，主要是为了好扩展
	random_int_secret_init();//初始化随机数生成池
}
```

### 3.4 driver_init
定义在msm/drivers/base/init.c中
```C
/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
void __init driver_init(void)
{
	/* These are the core pieces */
	devtmpfs_init();// 注册devtmpfs文件系统，启动kdevtmpfs进程
	devices_init();// 初始化驱动模型中的部分子系统，kset：devices 和 kobject：dev、 dev/block、 dev/char
	buses_init();// 初始化驱动模型中的bus子系统，kset：bus、devices/system
	classes_init();// 初始化驱动模型中的class子系统，kset：class
	firmware_init();// 初始化驱动模型中的firmware子系统 ，kobject：firmware
	hypervisor_init();// 初始化驱动模型中的hypervisor子系统，kobject：hypervisor

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	platform_bus_init();// 初始化驱动模型中的bus/platform子系统,这个节点是所有platform设备和驱动的总线类型，即所有platform设备和驱动都会挂载到这个总线上
	cpu_dev_init(); // 初始化驱动模型中的devices/system/cpu子系统,该节点包含CPU相关的属性
	memory_dev_init();//初始化驱动模型中的/devices/system/memory子系统,该节点包含了内存相关的属性，如块大小等
}

```
这个函数完成驱动子系统的构建，实现了Linux设备驱动的一个整体框架，但是它只是建立了目录结构，具体驱动的装载是在do_initcalls函数，之前有讲

kernel_init_freeable函数告一段落了，我们接着讲kernel_init中剩余的函数

### 3.5 free_initmem
定义在msm/arch/arm64/mm/init.c中中

```C
void free_initmem(void)
{
	poison_init_mem(__init_begin, __init_end - __init_begin);
	free_initmem_default(0);
}
```

所有使用__init标记过的函数和使用__initdata标记过的数据，在free_initmem函数执行后，都不能使用，它们曾经获得的内存现在可以重新用于其他目的。

### 3.6 flush_delayed_fput
定义在msm/arch/arm64/mm/init.c中,它执行的是delayed_fput(NULL)
```C
static void delayed_fput(struct work_struct *unused)
{
	LIST_HEAD(head);
	spin_lock_irq(&delayed_fput_lock);
	list_splice_init(&delayed_fput_list, &head);
	spin_unlock_irq(&delayed_fput_lock);
	while (!list_empty(&head)) {
		struct file *f = list_first_entry(&head, struct file, f_u.fu_list);
		list_del_init(&f->f_u.fu_list); //删除fu_list
		__fput(f); //释放struct file
	}
}
```
这个函数主要用于释放&delayed_fput_list这个链表中的struct file，struct file即文件结构体，代表一个打开的文件，系统中的每个打开的文件在内核空间都有一个关联的 struct file。

### 3.7 run_init_process
定义在msm/init/main.c中
```C
static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(init_filename,
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init); //do_execve就是执行一个可执行文件
}
```

run_init_process就是运行可执行文件了，从kernel_init函数中可知，系统会依次去找根目录下的init，execute_command，/sbin/init，/etc/init，/bin/init,/bin/sh这六个可执行文件，只要找到其中一个，其他就不执行。

Android系统一般会在根目录下放一个init的可执行文件，也就是说Linux系统的init进程在内核初始化完成后，就直接执行init这个文件，这个文件的源代码在platform/system/core/init/init.cpp，下一篇文章中我将从这个文件为入口，讲解Android系统的init进程。





