## Lab5: xv6 lazy page allocation

lab5主要是为sbrk()系统调用实现懒分配

os可以使用页表实现很多技巧，其中一个是对于用户空间堆内存的懒分配，xv6应用程序使用sbrk()系统调用来向内核申请堆内存；在未修改版本的xv6中，sbrk()分配物理页，并将其映射到进程的虚拟地址空间，对一个大的堆内存申请需求，内核需要一段时间去分配并且映射到页表；另外，一些程序会申请比实际使用更多的内存，或者在使用前去分配内存；为了使得sbrk()在这些情况下更快完成，更为复杂的内核实现懒分配；这时，sbrk()不实际分配物理内存，而是记录哪些用户地址被分配，并且在用户页表标记这些地址是非法的；当进程首次尝试访问懒分配的内存中任何一页时，CPU会产生页错误，内核处理该错误，为其分配物理内存，填充0，并且进行映射；在该lab中为xv6添加该特性

#### Eliminate allocation from sbrk() (easy)

##### 任务

完成lazy alloc首先需要在sbrk(n)系统调用实现中删除物理页分配代码，sbrk(n)系统调用增长进程n个字节的内存大小，返回新分配区域的首地址；新的sbrk(n)应该仅仅增加进程n个字节的内存大小，并返回之前的内存边界；由于不分配物理内存，所以需要删除growproc()函数

##### 过程

修改`sysproc.c/sys_sbrk`的实现，仅仅修改进程的sz的值，而不实际分配物理页，代码如下：

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  struct proc* p = myproc();
  addr = p->sz;
  
  if(n < 0) {
    uvmdealloc(p->pagetable, p->sz, p->sz + n);
  }
  p->sz += n;
  
  return addr;
}
```

#### Lazy allocation (moderate)

##### 任务

修改`trap.c`中的代码使得当用户地址发生页错误时，为页错误地址分配新的物理页，并进行映射

##### 过程

1. 创建`vm.c/lazy_alloc`函数

`lazy_alloc`函数根据出现页错误时的进程虚拟地址，为该进程分配物理页并进行映射

如果进程访问的虚拟地址超出进程内存大小（堆空间）边界或低于栈顶地址时，杀掉此进程

当分配物理页失败或映射物理页到用户页表失败时，同样杀掉此进程

```c
// Lazy alloc for sbrk
int
lazy_alloc(uint64 stval, struct proc *p)
{
  uint64 va = PGROUNDDOWN(stval);
  if(stval >= p->sz || stval < PGROUNDDOWN(p->trapframe->sp)) {
    //printf("lazy alloc error: va higher than sz or below user stack\n");
    p->killed = 1;
    return -1;
  }

  char *mem = kalloc();
  if(mem == 0){
    //printf("lazy alloc error: no more memory\n");
    p->killed = 1;
    return -1;
  }

  memset(mem, 0, PGSIZE);
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_U) != 0){
    kfree(mem);
    //printf("lazy alloc error: map error\n");
    p->killed = 1;
    return -1;
  }

  return 0;
}
```

2. 处理页错误陷入

当用户进程访问到某个堆空间处的虚拟地址，由于是懒分配，实际上并没有分配相应的物理页，cpu产生缺页中断，产生页错误陷入到内核后，应调用`lazy_alloc`为其分配物理页后，重新访问该地址

陷入的原因状态码被保存在`scause`寄存器中，13代表Load page fault、15代表Store page fault，因此当`scause`为13或15时，也即进程正在读取或写入虚拟地址时，实现懒分配并恢复访问

代码如下所示：

```c
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if (r_scause() == 13 || r_scause() == 15) {
    // lazy allocation
    // printf("page fault: stval=%p pid=%d\n", r_stval(), p->pid);
    uint64 stval = r_stval();
    lazy_alloc(stval, p);
  } else {
```

3. 修改`vm.c/uvmunmap`函数

由于是懒分配，当进程需要被释放，取消映射的页表时，有的物理页实际并不存在，因此需要修改取消映射的规则，当物理页不存在时略过

```c
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
      //panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      //panic("uvmunmap: not mapped");
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
```



#### Lazytests and Usertests (moderate)

##### 任务

使得将内核修改为lazy alloc后能够通过`lazytests`和`usertests`

##### 过程

1. 修改`vm.c/uvmcopy`

由于`fork`系统调用需要复制父进程的地址空间到子进程，父进程的一些地址空间的物理页并没有被分配，因此复制时应跳过不存在的物理页，不进行复制

```c
    if((pte = walk(old, i, 0)) == 0)
      continue;
      //panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      continue;
      //panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
```

2. 处理当进程调用如`read`、`write`系统调用时传递合法的虚拟地址参数，但该地址实际的物理页未分配的情况

`read`系统调用需要传递一个用户的虚拟地址指针，内核将文件数据复制到该用户指针指向的虚拟地址处，该地址如果是堆内存空间的地址，当所对应的物理页并没有分配时，会在`read`系统调用过程中出现错误，但不同于用户进程访问未分配内存的地址时出现的页错误；该错误应在`read`中处理，提前判断进程传进的用户指针参数所对应的物理页是否存在，如果不存在则分配；`write`、`pipe`系统调用也是类似的

当用户指针参数`p`不合法时，应对`read`返回表示错误的返回值，而不是杀掉该进程，因此应修改上述的`vm.c/lazy_alloc`代码为`vm.c/lazy_wr_alloc`：

```c
// Lazy alloc for write read and pipe
int
lazy_wr_alloc(uint64 va, struct proc *p)
{
  if(va >= p->sz || va < PGROUNDDOWN(p->trapframe->sp)) {
    //printf("lazy wr alloc error: va higher than sz\n");
    return -1;
  }

  char *mem = kalloc();
  if(mem == 0){
    //printf("lazy wr alloc error: no more memory\n");
    p->killed = 1;
    return -1;
  }

  memset(mem, 0, PGSIZE);
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_U) != 0){
    kfree(mem);
    //printf("lazy wr alloc error: map error\n");
    p->killed = 1;
    return -1;
  }

  return 0;
}
```

`sysfile.c/sys_read`修改如下：

```c
uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  struct proc *pro = myproc();
  for(uint64 va = PGROUNDDOWN(p); va < p + n; va += PGSIZE) {
    if(walkaddr(pro->pagetable, va) == 0) {
      if(lazy_wr_alloc(va, pro) < 0)
        return -1;
    }
  }

  return fileread(f, p, n);
}
```

`sysfile.c/sys_write`修改如下：

```c
uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  
  struct proc *pro = myproc();
  for(uint64 va = PGROUNDDOWN(p); va < p + n; va += PGSIZE) {
    if(walkaddr(pro->pagetable, va) == 0) {
      if(lazy_wr_alloc(va, pro) < 0)
        return -1;
    }
  }

  return filewrite(f, p, n);
}
```

`sysfile.c/sys_pipe`修改如下：

```c
  if(argaddr(0, &fdarray) < 0)
    return -1;
  
  for(uint64 va = PGROUNDDOWN(fdarray); va < PGROUNDUP(fdarray + 2*sizeof(fd0)); va += PGSIZE) {
    if(walkaddr(p->pagetable, va) == 0) {
      if(lazy_wr_alloc(va, p) < 0)
        return -1;
    }
  }

  if(pipealloc(&rf, &wf) < 0)
    return -1;
```

##### 运行结果

lazytests:

![image-20210410160211604](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210410160218.png)

usertests:

![image-20210410164834879](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210410164834.png)

#### 实验测试

![image-20210410164038207](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210410164038.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/lazy

