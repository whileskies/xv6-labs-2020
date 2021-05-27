## Lab7: Multithreading

lab7为三个多线程相关的任务：实现用户态线程、使用多线程加速程序、实现一个线程屏障。

#### Uthread: switching between threads

##### 任务

设计并实现一个用户态线程的上下文切换机制（其实类似于协程）。

##### 过程

1. 定义`context`结构，用于保存上下文寄存器

当从一个线程切换到另一个线程时，需要保存切换前线程的上下文，恢复切换后线程的上下文。在保存时可以参考xv6中内核线程的切换，只用保存被调用者保存的寄存器即可，因为需要调用者保存的寄存器都已经存储在线程的栈中了。

除此之外还应保存两个重要的寄存器：ra、sp，ra相当于保存了pc寄存器，用户恢复到之前切换的地方继续执行；sp保存了线程栈顶指针，每个线程都需要在栈中执行。

在`user/uthread.c `中定义`context`结构体如下：

```c
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};
```

将`context`结构体添加至`thread`结构体：

```c
struct thread {
  char       stack[STACK_SIZE]; /* the thread's stack */
  int        state;             /* FREE, RUNNING, RUNNABLE */

  struct context context;
};
```

2. 线程切换汇编

用户线程的切换汇编代码参考了(复制了)内核线程切换的汇编代码，代码如下：

```c
thread_switch:
	/* YOUR CODE HERE */
	sd ra, 0(a0)
	sd sp, 8(a0)
	sd s0, 16(a0)
	sd s1, 24(a0)
	sd s2, 32(a0)
	sd s3, 40(a0)
	sd s4, 48(a0)
	sd s5, 56(a0)
	sd s6, 64(a0)
	sd s7, 72(a0)
	sd s8, 80(a0)
	sd s9, 88(a0)
	sd s10, 96(a0)
	sd s11, 104(a0)

	ld ra, 0(a1)
	ld sp, 8(a1)
	ld s0, 16(a1)
	ld s1, 24(a1)
	ld s2, 32(a1)
	ld s3, 40(a1)
	ld s4, 48(a1)
	ld s5, 56(a1)
	ld s6, 64(a1)
	ld s7, 72(a1)
	ld s8, 80(a1)
	ld s9, 88(a1)
	ld s10, 96(a1)
	ld s11, 104(a1)
	ret    /* return to ra */
```

声明线程切换的函数：

```c
extern void thread_switch(struct context*, struct context*)
```

3. 线程初始化

线程初始化时，将ra设为函数地址，sp设为该线程的堆栈，等调度到该线程时，线程就可执行所绑定的函数，需要注意栈是从高地址向低地址增长的，代码如下：

```c
void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
  t->context.sp = (uint64)t->stack + STACK_SIZE;
  t->context.ra = (uint64)func;
}
```

4. 线程切换

当一个线程主动调用`thread_yield`函数时，放弃CPU的控制权，将其交给调度器，调度器函数找到下一个可执行的线程，切换到该线程进行执行，代码如下：

```c
if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
    thread_switch(&t->context, &next_thread->context);
} else
    next_thread = 0;
```

##### 运行结果

![image-20210527203123223](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210527203130.png)

#### Using threads

##### 任务

在hash表中使用多线程和锁，用于加快程序运行速度。在linux中使用`pthread`库来完成，并非xv6。

##### 过程

1. race condition

对hash表执行插入元素的代码如下所示：

```c
static void 
insert(int key, int value, struct entry **p, struct entry *n)
{
  struct entry *e = malloc(sizeof(struct entry));
  e->key = key;
  e->value = value;
  e->next = n;
  *p = e;
}
```

在插入元素时，首先通过对key进行hash找到相应的链表，将新插入的元素作为链表头，之前的链表接到新插入元素的后面。当单线程时肯定能正常执行，不会产生问题，但当多个线程并发的执行上面的`insert`函数时就会出现问题：两个线程同时创建新的`entry`，并将链表放到`entry`后部，之后一个线程先执行`*p = e`将自己的`entry`作为链表头，另一个线程再执行`*p = e`将自己的`entry`作为链表头，此时先执行的线程的修改被后执行的线程覆盖掉了，丢失了修改，也就没有成功插入新元素。

为解决上述多线程情况下的race condition，需要将`insert`操作作为一个原子操作，一次只能有一个线程执行，互斥锁正是用来解决此问题的。

2. 定义互斥锁

定义并初始化线程互斥锁，如下所示：

```c
// 全局变量
pthread_mutex_t lock;

// main函数中
pthread_mutex_init(&lock, NULL);
```

3. 为`insert`操作加锁

在`put`函数中为`insert`函数加互斥锁，由于`put`之前基本都是读操作，不用加锁，虽然`e->value = value;`是修改操作，但不会产生冲突，一定是后修改的线程会覆盖先修改的线程，也不用加锁，只有`insert`才需要加锁，代码如下所示：

```c
static 
void put(int key, int value)
{
  int i = key % NBUCKET;

  // is the key already present?
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    pthread_mutex_lock(&lock);
    insert(key, value, &table[i], table[i]);
    pthread_mutex_unlock(&lock);
  }
}
```

##### 运行结果

![image-20210527205109919](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210527205109.png)

可见使用两个线程时的速度为单线程的将近2倍，并且多线程情况下没有发生插入的丢失。

#### Barrier

##### 任务

使用`pthread`的条件等待实现线程屏障(barrier)：所有的线程都需要在该点等待直到所有其他线程也都到达了该点。

##### 过程

每一轮`barrier`结构维护当前到达屏障的线程数目，当未全部达到时，先到的线程调用`pthread_cond_wait`在条件上等待，当最后一个线程到达时，再调用`pthread_cond_broadcast`唤醒所有等待的线程，之后所有线程进行新一轮的执行。

在`barrier()`函数实现的过程中，同样涉及多个线程对于`barrier`共享数据结构的修改，因此应该加锁进行同步。

代码如下所示：

```c
static void 
barrier()
{
  pthread_mutex_lock(&bstate.barrier_mutex);
  bstate.nthread++;
  if (bstate.nthread < nthread) {
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
  } else {
    pthread_cond_broadcast(&bstate.barrier_cond);
    bstate.round++;
    bstate.nthread = 0;
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```

#### 实验测试

![image-20210527210418061](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210527210418.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/thread

