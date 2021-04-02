## Lab4: traps

Lab4为函数调用以及陷入相关的实验

#### RISC-V assembly (easy)

该部分为一系列RISC-V汇编相关的问题，阅读`user/call.asm`对应的`call.asm`汇编文件，回答下面问题：

1. 哪些寄存器包含函数的参数，例如，对于`main`调用`printf`函数，哪个寄存器存参数13？

对于RISC-V，前8个参数会放置在a0-a7寄存器，a2放置参数13，如下代码所示：

```assembly
void main(void) {
  1c:	1101                	addi	sp,sp,-32
  1e:	ec06                	sd	ra,24(sp)
  20:	e822                	sd	s0,16(sp)
  22:	1000                	addi	s0,sp,32
  printf("%d %d\n", f(8)+1, 13);
  24:	4635                	li	a2,13
  26:	45b1                	li	a1,12
  28:	00000517          	auipc	a0,0x0
  2c:	7f850513          	addi	a0,a0,2040 # 820 <malloc+0xea>
  30:	00000097          	auipc	ra,0x0
  34:	648080e7          	jalr	1608(ra) # 678 <printf>

```

2. `main`汇编中哪里调用了函数`f`和`g`？

由上述代码可以看到，`main`中直接得到了12，并放到了a1寄存器，可见编译器进行了优化，直接得到了结果

3. `printf`的地址在哪？

由汇编文件可以看到`printf`的地址为0x630，当做完alarm后，该汇编文件会发生变化，`printf`的地址也会变化

4. 当要进入`main`中`printf`函数，执行`jalr`指令后ra寄存器的值是多少？

ra应为函数调用中断点出的地址，也即`jalr`下一条指令的地址，为0x38

5. 运行下列代码：

```c
unsigned int i = 0x00646c72;
printf("H%x Wo%s", 57616, &i);
```

输出依赖于RISC-V是小端系统，如果RISC-V是大端，i应如何设置得到相同结果，是否需要改变57616的值？

小端即低字节放置在低地址，输出为："HE110 World"，如果为大端i为0x726c6400，57616不需要改变

6. 下列代码：

```c
printf("x=%d y=%d", 3);
```

`y=`将要输出什么，为什么会这样？

输出结果为x=3，但y是一个不确定的值，实际可能为a2寄存器的值

#### Backtrace (moderate)

##### 任务

在`kernel/printf.c`实现`backtrace()`函数，用于打印函数调用过程，在`sys_sleep`中插入该函数，之后运行测试

##### 过程

1. 内联汇编读取s0寄存器，即fp栈指针的值

代码如下所示，fp(s0)寄存器用于保存当前函数栈帧的首地址：

```c
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

2. 栈帧结构

如下所示，fp寄存器为当前栈帧的首地址，fp-8为上级函数的返回地址，fp-16为上级栈帧的首地址，一直沿着上级栈帧的地址，可以打印出整个栈的调用过程

```
Stack
                   .
                   .
      +->          .
      |   +-----------------+   |
      |   | return address  |   |
      |   |   previous fp ------+
      |   | saved registers |
      |   | local variables |
      |   |       ...       | <-+
      |   +-----------------+   |
      |   | return address  |   |
      +------ previous fp   |   |
          | saved registers |   |
          | local variables |   |
      +-> |       ...       |   |
      |   +-----------------+   |
      |   | return address  |   |
      |   |   previous fp ------+
      |   | saved registers |
      |   | local variables |
      |   |       ...       | <-+
      |   +-----------------+   |
      |   | return address  |   |
      +------ previous fp   |   |
          | saved registers |   |
          | local variables |   |
  $fp --> |       ...       |   |
          +-----------------+   |
          | return address  |   |
          |   previous fp ------+
          | saved registers |
  $sp --> | local variables |
          +-----------------+

```

3. `backtrace`函数

不断通过fp = fp-16获取栈帧地址，对于每个栈帧，打印上级函数的返回地址

```c
void
backtrace()
{
  uint64 cur_fp = r_fp();
  printf("backtrace:\n");
  for (uint64 fp = cur_fp; fp < PGROUNDUP(cur_fp); fp = *((uint64 *)(fp - 16)) ) {
    printf("%p\n", *((uint64 *)(fp - 8)));
  }
}
```

#### Alarm (hard)

##### 任务

该节你将为xv6添加对于进程使用CPU时间时能够周期性地发出警报的功能，这对于计算密集型进程限制使用CPU时间或者进程希望周期性地执行某个动作很有用；更进一步，你将实现一个初级形式的用户态中断/故障处理程序，和处理应用中的页错误类似

首先添加`sigalarm(interval, handler)`系统调用，如果程序调用`sigalarm(n, fn)`，则程序每消耗n个ticks，内核调用程序的`fn`函数，当`fn`返回，程序应该在之前中断的地方恢复执行；一个tick是xv6的一个计时单元，由硬件时钟生成中断；如果一个应用调用`sigalarm(0, 0)`，内核应停止周期性地执行alarm调用

##### 过程

1. 添加`sys_sigalarm`、`sys_sigreturn`两个系统调用定义

按之前lab添加系统调用的方式即可

2. 在`proc.h/struct proc`添加alarm相关的成员变量

如下所示，`alarm_ticks`为alarm的周期，`alarm_handler_addr`为alarm处理函数的地址，该地址为用户进程的虚拟地址，这两个由`sys_sigalarm`系统调用参数设置；`ticks`为当前进程消耗的CPU时间，`last_ticks`为上一次执行alarm处理函数的开始CPU时间，`alarm_regs`为执行处理函数时保存与需要恢复的寄存器组值，`alarm_running`用来标记是否该进程正在执行处理函数中

```c
  int alarm_ticks;                   // lab alarm
  uint64 alarm_handler_addr;         // lab alarm
  uint64 ticks;                      // lab alarm
  uint64 last_ticks;                 // lab alarm
  struct alarm_regs regs;            // lab alarm
  int alarm_running;                 // lab alarm
```

3. 初始化`proc`结构体alarm相关变量

在`proc.c/allocproc`函数中对上述定义的相关变量进行初始化，初始ticks为0

```c
  // Init ticks for lab alarm
  p->ticks = 0;
  p->last_ticks = 0;
  p->alarm_running = 0;
```

4. 添加`sys_sigalarm`系统调用实现

`sys_sigalarm`对进程`proc`结构体的`alarm_ticks`、`alarm_handler_addr`变量进行设置，同时设置`last_ticks`为当前`ticks`，也即从当前开始计时

```c
uint64 
sys_sigalarm(void)
{
  int ticks;
  uint64 handler_addr;

  if (argint(0, &ticks) < 0 || argaddr(1, &handler_addr) < 0)
    return -1;

  struct proc *p = myproc();
  p->alarm_ticks = ticks;
  p->alarm_handler_addr = handler_addr;
  p->last_ticks = p->ticks;

  return 0;
}
```

5. 保存与恢复上下文

当进程从当前运行地方切换到处理函数入口地址时，应保存切换时的CPU寄存器值，这里在`proc.h`定义`struct alarm_regs`结构体，其需要保存的寄存器基本和`trapframe`一致

```c
struct alarm_regs
{
  uint64 epc;
  uint64 ra;
  uint64 sp;
  .....
  uint64 s10;
  uint64 t5;
  uint64 t6;
};
```

当执行`sys_sigalarm`系统调用进入内核后，当前进程用户态的上下文，也即执行ecall指令时的状态，保存在进程结构体的`trapframe`中，由于`sys_sigalarm`系统调用返回后强制使得该进程跳转到了处理函数去执行，执行完成后在通过`sys_sigreturn`系统调用恢复，在这个处理函数执行过程中`trapframe`已经发生了很大变化，因此需要保证`sys_sigalarm`进入时的`trapframe`和`sys_sigreturn`返回时的`trapframe`一致，即可恢复到执行处理函数之前的位置继续执行

在`trap.c`中定义保存与恢复上下文如下所示：

```c
void
save_regs(struct proc *p)
{
  p->regs.epc = p->trapframe->epc;
  p->regs.ra = p->trapframe->ra;
  p->regs.sp = p->trapframe->sp;
  .........
  p->regs.t3 = p->trapframe->t3;
  p->regs.t4 = p->trapframe->t4;
  p->regs.t5 = p->trapframe->t5;
  p->regs.t6 = p->trapframe->t6;
}

void restore_regs(struct proc *p)
{
  p->trapframe->epc = p->regs.epc;
  p->trapframe->ra = p->regs.ra;
  p->trapframe->sp = p->regs.sp;
  p->trapframe->gp = p->regs.gp;
  .......
  p->trapframe->t5 = p->regs.t5;
  p->trapframe->t6 = p->regs.t6;
}
```

6. 处理时钟中断

每当进程因为时钟中断陷入后，进程的CPU时间`ticks`增加，当alarm处理程序未在运行，并且设置了alarm周期时间，则当到期后就开始执行处理函数

首先`p->last_ticks = p->ticks`设置最后调用处理函数的开始时间为当前`ticks`

`save_regs(p)`保存了将要调用处理函数之前的CPU寄存器状态

`p->trapframe->epc = p->alarm_handler_addr`将陷入后的返回地址设置为处理函数的地址，当`trampoline.S/userret`最后的`sret`指令执行后，即将PC设置为了处理函数地址，也即执行该处理函数

`p->alarm_running = 1`表示该进程的处理函数正在执行

```c
  // give up the CPU if this is a timer interrupt.
  // lab alarm
  if(which_dev == 2) {
    p->ticks++;
    if (p->alarm_ticks != 0 && p->alarm_running == 0) {
      if (p->last_ticks + p->alarm_ticks <= p->ticks) {
        p->last_ticks = p->ticks;

        save_regs(p);
        p->trapframe->epc = p->alarm_handler_addr;
        
        p->alarm_running = 1;
      }
    }
```

7. 添加`sys_sigreturn`系统调用实现

在`sysproc.c`中添加`sys_sigreturn`实现，主要作用是用户的alarm处理函数执行后，恢复到处理函数执行前的状态

```c
uint64 
sys_sigreturn(void)
{
  struct proc *p = myproc();
  restore_regs(p);
  p->alarm_running = 0;

  return 0;
}
```

##### 运行结果

![image-20210402142321233](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210402142321.png)

#### 实验测试

![image-20210402142613027](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210402142613.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/traps