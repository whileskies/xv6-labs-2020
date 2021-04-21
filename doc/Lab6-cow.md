## Lab6: Copy-on-Write Fork for xv6

lab6实现`fork()`的COW

虚拟内存提供一个中间层：内核通过使得页表项非法或者只读来拦截内存引用，导致页错误，继而通过改变页表项修改实际的物理地址；计算机系统领域有句名言：计算机系统的任何问题都可以通过加一个中间层来解决；lazy lab就是一个例子，该实验实现了另一个例子：写时复制fork

#### 问题

xv6中的`fork()`系统调用会拷贝父进程的用户内存空间到子进程；如果父进程非常大，拷贝可能会消耗很长时间，甚至，拷贝过程通常产生大的浪费，比如`fork()`调用通常会伴随着子进程调用`exec()`并清除掉所拷贝的内存，很可能根本就没有使用过这些内存；另一方面，如果父进程与子进程共享相同的物理页，当其中一个希望写该物理页时，需要拷贝该物理页

#### 解决方案

写时复制(COW) `fork()`的目标是推迟分配和拷贝物理页直到真正实际需要拷贝时

`COW fork()`仅仅为子进程创建页表，并且使得页表项中的用户地址指向父进程相应的物理页；`COW fork()`标记父子进程的页表项均为只读的（不可写的）；当有一方尝试写这些COW页时，CPU会产生页错误；内核页错误处理程序处理页错误，为出错进程分配物理页，复制原始页数据到新的物理页，修改相应页表项指向新物理页，这时将相应页表项标记为可写的；当页错误处理程序返回，用户进程可以正常写所复制的页

`COW fork()`释放物理页时有些棘手，一个物理页可能会被多个进程的页表所引用，应该当最后一个引用取消时才能释放

#### Implement copy-on write(hard)

##### 任务

完成写时复制`fork()`，修改后能够通过`cowtest`和`usertests`

##### 过程

1. 引用计数实现

由上所述，COW时一个物理页可能会被多个进程的页表引用，如一个进程调用`fork()`，子进程再次调用`fork()`...，那么只有没有进程引用该物理页时该物理页才能被释放，可以使用引用计数解决，每个物理页对应一个数值，表示当前被引用的进程个数，当个数为0时释放

实现时可以维护一个引用计数数组，数组的第`i`项表示从`KERNBASE`开始的第`i`个物理页的引用计数，最大物理页的个数为`(PHYSTOP - KERNBASE) / PGSIZE`，在`kalloc.c`中定义引用数组，由于是全局变量，初始值均为0：

```c
uint16 pgs_rfc[(PHYSTOP - KERNBASE) / PGSIZE];
```

给出一个物理地址`get_pg_rfc()`函数获取相应物理页的引用计数，`set_pg_rfc()`设置物理页的引用计数：

```c
uint16
get_pg_rfc(uint64 pa)
{
  return pgs_rfc[(pa - KERNBASE) >> 12];
}

void
set_pg_rfc(uint64 pa, uint16 rfc)
{
  pgs_rfc[(pa - KERNBASE) >> 12] = rfc;
}
```

修改`kalloc.c/kfree()`，仅当pa对应的物理页引用计数 <= 1时，才释放物理页，当超过一个进程引用物理页时，仅仅将引用数 -1：

```c
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  uint16 ref = get_pg_rfc((uint64)pa);
  if (ref > 1) {
    set_pg_rfc((uint64)pa, ref - 1);
    return;
  }
```

修改`kalloc.c/kalloc`，当分配一个物理页时，该物理页的引用计数为1：

```c
  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    set_pg_rfc((uint64)r, 1);
  }
```

2. 修改`vm.c/uvmcopy`函数为COW

`fork()`函数通过调用`uvmcopy()`函数复制父进程的所有物理页，因此需要将其修改为父子进程共享同一物理页，并且相应的页表项均为只读与使用COW标记，同时物理页的引用计数+1

页表项COW标记在PTE的第8位（RSW预留位）设置，在`riscv.h`中添加如下宏定义：

```c
#define PTE_COW (1L << 8)
```

修改后的`uvmcopy`函数代码为：

```c
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    *pte = *pte & ~PTE_W;
    *pte = *pte | PTE_COW;
    set_pg_rfc(pa, get_pg_rfc(pa) + 1);
    if(mappages(new, i, PGSIZE, pa, (flags & ~PTE_W) | PTE_COW) != 0) {
      set_pg_rfc(pa, get_pg_rfc(pa) - 1);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 0);
  return -1;
}
```

3. `cow()`写时复制函数实现

当`COW fork()`后，父进程或子进程写被共享的物理页时，需要分配一新物理页，复制被共享的物理页，新物理页对应的页表项标记位可写，被共享物理页引用计数 -1，当引用计数为0时，释放被共享物理页

在`vm.c`中添加`cow()`函数，代码如下：

```c
int
cow(uint64 va, struct proc *p)
{
  pte_t *pte = walk(p->pagetable, va, 0);
  if(*pte & PTE_COW) {
    char *mem;
    uint64 pa = PTE2PA(*pte);

    if((mem = kalloc()) == 0) 
      return -1;

    memmove(mem, (char*)pa, PGSIZE);
    uint16 rfc = get_pg_rfc(pa);
    rfc--;
    set_pg_rfc(pa, rfc);
    if (rfc == 0) {
      kfree((char *)pa);
    }

    pte_t newpte = PA2PTE(mem);
    newpte = ((newpte | PTE_FLAGS(*pte)) | PTE_W) & ~PTE_COW;
    *pte = newpte;
  } else 
    return -2;

  return 0;
}
```

4. 调用`cow()`

当父进程或子进程写入数据到只读的共享页时，发生页错误，`trap.c/usertrap()`函数处理该页错误，当错误码为15时，执行COW，也即`cow()`函数，trap返回后会写入到合法的物理页，代码如下：

```c
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 15) {
    uint64 stval = r_stval();
    if (cow(stval, p) != 0) {
      p->killed = 1;
    }
  } else {
```

当父进程或子进程调用如`read()`之类的系统调用，将内核数据写入用户地址空间，如果该地址实际的物理页为共享页的话，也会发生错误，但是出错时内核正在执行系统调用，不会产生`usertrap`，因此需要在`vm.c/copyout()`中增加COW的代码：

```c
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    cow(va0, myproc());
    pa0 = walkaddr(pagetable, va0);

    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
```

5. 修改引用计数时加锁

当修改某物理页引用计数时分为两步：`set_pg_rfc(pa, get_pg_rfc(pa) + 1)`，首先通过`get_pg_rfc()`函数获取某物理页的引用计数，之后再通过`set_pg_rfc()`来修改引用计数，当多个进程同时修改同一物理页的引用计数时，会发生`race conditons`，如果不加锁，会造成物理页的内存泄露

举个例子，如果有3个进程共享同一物理页，那么该物理页的引用计数为3，当其中2个进程同时调用`kfree()`函数释放物理页，在`kfree()`代码中，2个进程同时执行`uint16 ref = get_pg_rfc((uint64)pa)`，得到当前引用计数为3，之后一个进程先执行`set_pg_rfc((uint64)pa, ref - 1)`将该页引用计数变为2，再之后另一个进程同样执行`set_pg_rfc((uint64)pa, ref - 1)`将该页引用计数变为2，这样最终该页引用计数变为2（正确情况应为1），丢失了一次更新，实际上之后只有1个进程共享该物理页，当该进程也调用`kfree()`释放物理页时，引用计数从2变为1，而不是直接释放该物理页，最终造成该物理页的泄露，一直不会被回收

```c
void
kfree(void *pa)
{
  struct run *r;
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&rfc_lock);
  uint16 ref = get_pg_rfc((uint64)pa);
  if (ref > 1) {
    set_pg_rfc((uint64)pa, ref - 1);
    release(&rfc_lock);
    return;
  }
  release(&rfc_lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}
```

因此，为解决并发出现的问题，应当把获取引用计数、修改引用计数这两步操作作为一个原子操作，一次只能有一个CPU或进程执行该临界区代码

首先在`kalloc.c`定义页引用的自旋锁：

```c
struct spinlock rfc_lock;
```

在`kalloc.c/kinit()`函数中初始化该自旋锁：

```c
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&rfc_lock, "pgs_rfc");
  freerange(end, (void*)PHYSTOP);
}
```

在`kalloc.c/kfree()`函数中加锁：

```c
  acquire(&rfc_lock);
  uint16 ref = get_pg_rfc((uint64)pa);
  if (ref > 1) {
    set_pg_rfc((uint64)pa, ref - 1);
    release(&rfc_lock);
    return;
  }
  release(&rfc_lock);
```

在`vm.c/uvmcopy()`函数中加锁：

```c
    acquire(&rfc_lock);
    set_pg_rfc(pa, get_pg_rfc(pa) + 1);
    release(&rfc_lock);
    if(mappages(new, i, PGSIZE, pa, (flags & ~PTE_W) | PTE_COW) != 0) {
      acquire(&rfc_lock);
      set_pg_rfc(pa, get_pg_rfc(pa) - 1);
      release(&rfc_lock);
      goto err;
    }
```

在`vm.c/cow`中加锁：

```c
    acquire(&rfc_lock);
    uint16 rfc = get_pg_rfc(pa);
    rfc--;
    set_pg_rfc(pa, rfc);
    release(&rfc_lock);
    if (rfc == 0) {
      kfree((char *)pa);
    }
```

实际上这里加锁的粒度比较大，是对访问整个引用数组时加锁，实际上只需对访问特定物理页的引用计数时加锁，但是这样需要的锁过多

#### 实验测试

![image-20210413175042082](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210413175049.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/cow