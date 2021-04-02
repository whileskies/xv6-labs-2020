## Lab3: page tables

Lab3为3个页表相关实验，用于理解os的页表机制

#### Print a page table (easy)

##### 任务

按层次输出多级页表的页表项，定义函数`vmprint()`，参数为level-2级页表地址`pagetable_t`，并输出pid为1的进程的用户页表

可以参照`freewalk`进行递归遍历

```c
// Print page table
void
vmprint_level(pagetable_t pagetable, int level)
{
  if (level < 0) return;
  if (level == 2)
    printf("page table %p\n", pagetable);
  
  level = level - 1;
  
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      if (level == 1)
        printf("..");
      else if (level == 0)
        printf(".. ..");
      else if (level == -1)
    printf(".. .. ..");
      printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));
      uint64 child = PTE2PA(pte);
      vmprint_level((pagetable_t)child, level);
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  vmprint_level(pagetable, 2);
}
```

##### 运行结果

![image-20210324150310221](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210324150317.png)

#### A kernel page table per process (hard)

##### 任务

实现每个进程一个内核页表

xv6当位于内核态时使用的是内核页表，xv6有一个内核页表用于对os虚拟地址进行映射，该页表对物理内存进行一一映射，也即内核虚拟地址x映射为物理地址x，如下图所示：

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210324151158.png" alt="image-20210324151158499" style="zoom:50%;" />

同时xv6对于每个进程也有个用户页表，用于映射用户进程的地址空间，用户虚拟地址空间从0开始，如下图所示：

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210324151523.png" alt="image-20210324151523096" style="zoom:50%;" />

然而内核页表不含有进程用户地址空间的映射（如代码段、数据段、堆段、栈段），os在内核态执行时对于这些用户地址是不合法的，因此，当内核在系统调用中需要使用作为参数的用户指针进行返回数据时，内核必须先要将用户指针根据用户页表转换为物理地址。本节以及下一节实验目标是允许内核去直接解引用用户指针所指的地址，而不再先通过用户用户页表转换

本节任务是修改内核，使得每个进程有一个内核页表的副本，当在内核执行时，使用该内核页表副本，而不是原本的内核页表

##### 过程

1. 在进程数据结构中增加内核页表域，位于`proc.h`

```c
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  struct proc *parent;         // Parent process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  pagetable_t kernel_pagetable; // lab3 user kernel pagetable
};
```

2. 参考`kvminit`定义`user_kvminit`初始化进程内核页表

```c
// Lab3: A kernel page table per process.
pagetable_t
user_kvminit()
{
  pagetable_t user_kernel_pt = (pagetable_t) kalloc();
  if (user_kernel_pt == 0) 
    return 0;
  memset(user_kernel_pt, 0, PGSIZE);
  

  // uart registers
  mappages(user_kernel_pt, UART0, PGSIZE, UART0, PTE_R | PTE_W);

  // virtio mmio disk interface
  mappages(user_kernel_pt, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W);

  // CLINT
  // mappages(user_kernel_pt, CLINT, 0x10000, CLINT, PTE_R | PTE_W);

  // PLIC
  mappages(user_kernel_pt, PLIC, 0x400000, PLIC, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  mappages(user_kernel_pt, KERNBASE, (uint64)etext-KERNBASE, KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  mappages(user_kernel_pt, (uint64)etext, PHYSTOP-(uint64)etext, (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  mappages(user_kernel_pt, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X);

  return user_kernel_pt;
}
```

3. 初始化进程内核页表

在`proc.c/allocproc`函数中当分配一个进程时，初始化进程内核页表

```c
  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Init kernel page table per process.
  p->kernel_pagetable = user_kvminit();
  if (p->kernel_pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
```

4. 每个进程内核页表映射进程的内核栈

每个进程有两个栈，一个是用户栈，当进程在用户态执行时使用该栈，当该进程陷入后，使用该进程的内核栈，在之前只有一个内核页表时，`proc.c/procinit`函数中分配进程内核栈物理空间，内核页表对进程内核栈进行映射，如下代码所示：

```c
// Allocate a page for the process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
char *pa = kalloc();
if(pa == 0)
  panic("kalloc");
uint64 va = KSTACK((int) (p - proc));
kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
p->kstack = va;
```

当每个进程一个内核页表时，进程陷入后使用的是该进程的内核页表，运行的栈为该进程的内核栈，因此进程内核页表应对进程内核栈进行映射，将`proc.c/procinit`的上述代码移动到`/proc.c/allocproc`中去，并且被映射的页表改为进程内核页表

```c
  // Allocate a page for the process's kernel stack.
  // Map it high in memory, followed by an invalid
  // guard page.
  char *pa = kalloc();
  if(pa == 0)
    panic("kalloc");
  uint64 va = KSTACK((int) (p - proc));
  mappages(p->kernel_pagetable, va, PGSIZE, (uint64)pa, PTE_R | PTE_W);
  p->kstack = va;
```

5. 修改`proc.c/scheduler`

使得切换进程时应用该内核页表，当进程通过`switch`切换回去时，该过程为陷入的返回过程，在`trap.c/usertrapret`中，有保存当前寄存器页表的操作：

```c
  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid(
```

若在`switch`之前将寄存器页表从内核页表切换为进程内核页表，则当进程返回用户态并再次陷入后就会使用进程内核页表了，代码修改如下：

```c
p->state = RUNNING;
c->proc = p;

w_satp(MAKE_SATP(p->kernel_pagetable));
sfence_vma();

swtch(&c->context, &p->context);
```

如果没有进程需要调度时，仍然使用内核页表

```c
if(found == 0) {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();

  intr_on();
  asm volatile("wfi");
}
```

6. 释放进程

当需要释放进程时，同时也应释放该进程的内核栈，进程内核页表，但不应该释放进程内核页表所映射的物理地址

```c
if (p->kstack) {
  pte_t *pte = walk(p->kernel_pagetable, p->kstack, 0);
  uint64 pa = (uint64)PTE2PA(*pte);
  kfree((void *)pa);  
}

if (p->kernel_pagetable)
  free_pro_kernel_pagetable(p->kernel_pagetable);
p->kernel_pagetable = 0
```

7. 释放进程内核页表

递归释放进程内核页表，当不释放页表所映射的物理页

```c
// Free process kernel pagetable for lab3
void
free_pro_kernel_pagetable(pagetable_t pagetable)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
	if ((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0) {
      uint64 child = PTE2PA(pte);
      free_pro_kernel_pagetable((pagetable_t)child);
      pagetable[i] = 0;
	}
  }
  kfree((void*)pagetable);
}
```

8. 修改bug

将`vm.c/kvmpa`中的`pte = walk(kernel_pagetable, va, 0);`修改为如下代码：

```c
// pte = walk(kernel_pagetable, va, 0);
// process kernel pagetable has the map on the process kernel stack
pte = walk(myproc()->kernel_pagetable, va, 0);
if(pte == 0)
  panic("kvmpa")
```

9. 其他问题

在使用`usertests`程序进行测试时，会遇到有时能全通过，有时出现panic，可以在`switch`之后再切换回内核页表，如下图，调度器使用的是内核栈，其他进程陷入时使用的是进程内核栈，内核页表和进程内核页表映射不完全一致

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210324162457.png" alt="image-20210324162457924" style="zoom:67%;" />

```c
w_satp(MAKE_SATP(p->kernel_pagetable));
sfence_vma();

swtch(&c->context, &p->context);

kvminithart();

c->proc = 0;
```

##### 运行结果

![image-20210324162943188](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210324162943.png)

#### Simplify copyin/copyinstr (hard)

##### 任务

内核的`copyin`函数读取用户地址指针指向的内存，因此需用通过进程的用户页表转换为物理地址来访问。实验该部分的任务是给每个进程的内核页表添加该进程虚拟地址的映射，以允许`copyin`可以直接解引用用户指针

##### 过程

主要思路是在对进程用户页表进行映射的同时，将用户虚拟地址映射到用户内核页表

1. 替换`copyin`与`copyinstr`实现

```c
// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable, dst, srcva, max);
}
```

2. 修改`vm.c/uvmcopy`，对用户页表映射时，同时映射用户内核页表

```c
int
uvmcopy(pagetable_t old, pagetable_t new, pagetable_t kernelpt, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0 ||
      mappages(kernelpt, i, PGSIZE, (uint64)mem, flags & ~PTE_U) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  uvmunmap(kernelpt, 0, i / PGSIZE, 0);
  return -1;
}
```

3. 修改`vm.c/uvmalloc`，对用户页表映射时，同时映射用户内核页表

```c
uint64
uvmalloc(pagetable_t pagetable, pagetable_t kernelpt, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      uvmdealloc_nofree(kernelpt, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0 ||
       mappages(kernelpt, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R)) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      uvmdealloc_nofree(kernelpt, a, oldsz);
      return 0;
    }
  }
  return newsz;
}
```

4. 修改`vm.c/uvminit`

```c
void
uvminit(pagetable_t pagetable, pagetable_t kernelpt, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  mappages(kernelpt, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X);
  memmove(mem, src, sz);
}
```

5. 修改`proc.c/fork`

fork复制父进程的地址空间到子进程，复制用户页表同时，将其映射到子进程内核页表

```c
if(uvmcopy(p->pagetable, np->pagetable, np->kernel_pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
}
```

6. 修改`proc.c/exec`

创建进程时需要对用户虚拟地址进行映射，应同时映射到进程内核页表

因为此时正使用用户内核页表，释放旧的用户内核页表之前，应重新设置页表寄存器为新的用户内核页表，同时刷新TLB

![image-20210330094709357](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210330094716.png)

![image-20210330094737962](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210330094738.png)

7. 修改`sysproc.c/sys_sbrk`

防止在用户内核页表中的用户虚拟地址超过PLIC限制

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if (PGROUNDUP(addr + n) >= PLIC) {
    return -1;
  }
  if(growproc(n) < 0)
    return -1;
  return addr;
}
```

8. 复制并修改`vm.c/walkaddr`函数为`vm.c/kernelpt_walkaddr`

由于`walkaddr`函数只能转换用户页表的虚拟地址，因此复制一份可以转换内核页表的虚拟地址

```c
// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can be used to look up kernel page.
uint64
kernelpt_walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}
```

9. 复制并修改`vm.c/uvmdealloc函数为`vm.c/uvmdealloc_nofree`

由于`uvmdealloc`函数用于取消用户页表的映射，并删除物理页，但是用户内核页表取消映射时不能删除物理页，因此修改此函数

```c
uint64
uvmdealloc_nofree(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
  }

  return newsz;
}
```

#### 实验测试

![image-20210330100558882](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210330100558.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/pgtbl

