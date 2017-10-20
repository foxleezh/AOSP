## 前言

我们都知道Android系统的第一个进程叫init进程，它是Linux系统的第一个用户进程，这个进程会孵化出许多重要的进程，其中一个就是Zygote进程，这个我会在下一篇文章中讲到。

init进程作为天字第一号进程，自然有它的特别之处，本文将详细讲解它的前世今生，主要讲两个方面：

- init进程由哪里来
- init进程做了些什么

本文涉及到的文件
```
msm/arch/arm64/kernel/head.S
msm/init/main.c
msm/kernel/rcutree.c
msm/kernel/fork.c
msm/mm/mempolicy.c
msm/kernel/cpu/idle.c
platform/core/init/init.cpp
```

## 一、init进程前世

我们讲说init进程是Android的第一个进程，它的进程号是1，其实这种说法不准确，因为既然进程号是1，那么有没有进程号是0的进程呢，其实是有的。

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

### 1.1 rest_init

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

#### 1.1.1 rcu_scheduler_starting
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

#### 1.1.2 kernel_thread
定义在msm/kernel/fork.c
```C
/*
 * Create a kernel thread.
 */
pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags)
{
	return do_fork(flags|CLONE_VM|CLONE_UNTRACED, (unsigned long)fn,
		(unsigned long)arg, NULL, NULL);
}
```
do_fork方法用于创建进程，它首先调用copy_process()创建新进程，然后调用wake_up_new_task()将进程放入运行队列中并启动新进程。
kernel_thread的第一个参数是一个方法引用，会直接调用，第三个参数是创建进程的方式，具体如下：

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


#### 1.1.3 kernel_init
定义在msm/init/main.c

这个函数比较重要，我将放在本节末尾重点讲，这个方法首先调用kernel_init_freeable方法
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

#### 1.1.4 numa_default_policy
定义在msm/mm/mempolicy.c
```C
/* Reset policy of current process to default */
void numa_default_policy(void)
{
	do_set_mempolicy(MPOL_DEFAULT, 0, NULL); //设定NUMA系统的内存访问策略为MPOL_DEFAULT
}
```
#### 1.1.5 kthreadd
定义在msm/kernel/kthread.c中

kthreadd进程我将重点讲，它是内核中重要的进程，负责内核进程的调度和管理，内核进程基本都是以它为父进程的
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
		 /*  首先将线程状态设置为 TASK_INTERRUPTIBLE, 如果当前
            没有要创建的线程则主动放弃 CPU 完成调度.此进程变为阻塞态*/
		set_current_state(TASK_INTERRUPTIBLE);
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
kthreadd方法的作用就是循环地从kthread_create_list链表中取出要创建的线程，然后执行create_kthread方法，直到kthread_create_list为空，让出CPU,进入睡眠，我们来看下create_kthread方法
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
其实这里面就是调用kernel_thread方法创建进程，然后执行kthread方法，注意不要搞混了，之前那个方法叫kthreadd，接下来看看kthread方法
```C
static int kthread(void *_create)
{
	/* Copy data: it's on kthread's stack */
	struct kthread_create_info *create = _create;  // create 就是之前kthreadd方法循环取出的 kthread_create_info
	int (*threadfn)(void *data) = create->threadfn; //新进程工作函数
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
	complete(&create->done); //表示进程创建完毕
	schedule(); //让出CPU，注意这里并没有执行新进程的threadfn方法就直接进入睡眠了，然后等待进程被手动唤醒，然后才执行threadfn

	ret = -EINTR;

	if (!test_bit(KTHREAD_SHOULD_STOP, &self.flags)) {
		__kthread_parkme(&self);
		ret = threadfn(data);
	}
	/* we can't just return, we must preserve "self" on stack */
	do_exit(ret);
}
```

**kthreadd进程小结** 

kthreadd进程由idle通过kernel_thread创建，并始终运行在内核空间, 负责所有内核线程的调度和管理，它的任务就是管理和调度其他内核线程kernel_thread, 会循环执行一个kthreadd的函数，该函数的作用就是运行kthread_create_list全局链表中维护的kthread, 当我们调用kernel_thread创建的内核线程会被加入到此链表中，因此所有的内核线程都是直接或者间接的以kthreadd为父进程

我们在内核中通过kernel_create或者其他方式创建一个内核线程, 然后kthreadd内核线程被唤醒,　来执行内核线程创建的真正工作，新的线程将执行kthread函数, 完成创建工作，创建完毕后让出CPU，因此新的内核线程不会立刻运行．需要手工 wake up, 被唤醒后将执行自己的真正工作函数

#### 1.1.6 rcu_read_lock & rcu_read_unlock
定义在include/linux/rcupdate.h和msm/kernel/rcupdate.c中

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

#### 1.1.7 find_task_by_pid_ns
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
http://blog.csdn.net/gatieme/article/details/51484562
http://blog.csdn.net/xichangbao/article/details/52938240




