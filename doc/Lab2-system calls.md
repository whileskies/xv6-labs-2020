## Lab2: system calls

Lab2为xv6添加一些系统调用，以便了解系统调用过程

#### System call tracing (moderate)

**任务：**添加一个`trace`系统调用，参数为系统调用号掩码，用于追踪系统调用的路径以及返回值

##### 过程：

1. 首先为`proc.h`中的`proc`结构体添加一个跟踪掩码字段：

```c
// these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  int trace_mask;              // trace syscall mas
```

2. 为给系统调用添加系统调用号，如`syscall.h`、`syscall.c`文件中：

```c
#define SYS_trace  22

[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_trace]   sys_trace,
```

3. 在内核系统调用出口拦截系统调用名称以及返回值，需要一个字符串数组将系统调用号转为系统调用名称：

```c
static char *syscall_name[] = {
        "", "fork", "exit", "wait", "pipe", "read", "kill", "exec", "fstat", "chdir", "dup",
        "getpid", "sbrk", "sleep", "uptime", "open", "write", "mknod", "unlink", "link", "mkdir",
        "close", "trace"
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num]();

    if (p->trace_mask & (1 << num)) {
      printf("%d: syscall %s -> %d\n", p->pid, syscall_name[num], p->trapframe->a0);
    }
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
```

**运行结果：**

```c
uint64
sys_trace()
{
  int mask;

  if (argint(0, &mask) < 0)
      return -1;

  struct proc *pro = myproc();
  printf("trace pid: %d\n", pro->pid);
  pro->trace_mask = mask;

  return 0;
} 
```

5. 进程清除时也应清除相应掩码 `proc.c/freeproc`：

```c
p->chan = 0;
p->killed = 0;
p->xstate = 0;
p->state = UNUSED;

// trace
p->trace_mask = 0;
```

6. fork时子进程也复制到该掩码 `proc.c/fork`：

```c
pid = np->pid;

np->state = RUNNABLE;

// trace
np->trace_mask = p->trace_mask;

release(&np->lock)
```

**运行结果：**

![image-20210322140342934](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210322140342.png)

#### Sysinfo (moderate)

**任务：**实现sysinfo系统调用用于获取当前的空闲内存字节数以及状态为`UNUSED`的进程数

**过程：**

1. 获取空闲内存字节数

内存以页为单位进行分配和释放，每个空闲内存将其作为`struct run`结构体，最低的以`struct run`结构体指针大小的字节为next域，指向下一空闲内存页的地址

实现时遍历空闲页列表即可，当run指针为NULL时停止

```c
// Get the number of bytes of free memory
uint64
get_free_memory()
{
  struct run *r;
  uint64 pages = 0;

  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r) {
    pages++;
    r = r->next;   
  }
  release(&kmem.lock);

  return pages * PGSIZE;
}
```

2. 获取UNUSED进程数目

遍历所有的proc结构，统计`UNUSED`状态的数目

```c
// Get the num of proccesses
uint64
get_proccesses_num()
{
  struct proc *p;
  uint64 num = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p->state != UNUSED) 
      num++;
  }

  return num;
}
```

3. 实现sysinfo系统调用

参考`sys_fstat`等函数参数的传递以及指针类型数据复制的方式，将内核的数据复制到参数指针所指向的该进程用户态内存区域：

```c
uint64
sys_sysinfo()
{
  uint64 param;
  if(argaddr(0, &param) < 0)
    return -1;
  
  struct sysinfo info;
  info.freemem = get_free_memory();
  info.nproc = get_proccesses_num();

  struct proc *p = myproc();
  if (copyout(p->pagetable, param, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}
```

#### 实验测试

当在win10上使用`git clone`并完成实验后，使用ubuntu虚拟机运行测试，读取README文件时和要求的不一致，若在ubuntu上`git clone`，则可以通过测试，推测是不同os换行的符号不同，git进行了转换，下图为在ubuntu上`git clone`并复制README文件到项目后的运行结果

此外每个测试python脚本第一行`#!/usr/bin/env python`换行符在win下也应切换为LF格式（原本为CRLF）才可在ubuntu虚拟机上正常测试

![image-20210322142413089](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210322142413.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/syscall