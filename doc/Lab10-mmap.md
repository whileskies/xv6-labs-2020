## Lab10: mmap

Lab10需要为xv6实现简易版的mmap和munmap系统调用，mmap允许对进程的地址空间进行详细控制，可以用来进程间共享内存、将文件映射到进程地址空间等，本lab仅仅实现内存映射文件。

#### mmap 

##### 任务

mmap的接口格式如下：

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
```

在本lab中假设addr总是0，由内核决定所映射文件的虚拟地址，当返回0xffffffffffffffff 代表失败；length是映射的字节数，可能和文件大小不一致；prot表示被映射的内存是否可读、可写、可执行，可以假设prot是可读、可写或可读可写的；flags要么是MAP_SHARED，表示对于所映射内存的修改应该写回文件，要么是MAP_PRIVATE，表示不用写回文件，不用考虑其他的bits；fd是被映射文件的文件描述符；offset是文件的偏移，可以假定总为0（映射文件时总是从文件起始进行映射）。

如果多个进程映射同一MAP_SHARED文件，可以不用实现共享同一物理页。

munmap的接口格式如下：

```c
int munmap(uint64 addr, int length)
```

munmap应该移除在参数指定范围的mmap映射，如果进程修改了映射的内存，并且是MAP_SHARED的，修改应该首先写回文件；可以假定munmap取消映射的范围要么起始于映射区域的开始，要么结束与映射区域的结尾，要么整个区域，也即不会是整个映射区域中打一个孔，将剩余的映射区域分为两个部分。

##### 实现

1. 定义VMA

一个进程可以多次调用mmap映射多个文件，因此进程应记录每一次所映射的虚拟地址区域，定义vma代表进程虚拟地址映射的一个区域，字段包括是否被使用、区域的起始地址、结束地址、文件描述符、权限等。

每个进程包含多个VMA，最大为16个，代码如下所示：

```c
// param.h
#define NVMA         16    // maximum number of vma

// proc.h
struct vma {
  int used;
  uint64 addr;
  int length;
  int permissions;
  int flags;
  int offset;
  struct file *f;
  uint64 start;
  uint64 end;
};

struct proc {
  // 省略
  char name[16];               // Process name (debugging)
  struct vma vmas[NVMA];       // Virtual Memory Area
};
```

2. 实现mmap

`mmap()`实现为一系统调用，首先读取系统调用参数，如果该mmap是MAP_SHARED的，但是映射内存标志为不可写的，则返回错误。

从VMA数组中寻找一未使用的VMA，之后确定映射的起始地址，起始地址为已使用的VMA中最大结束地址，如果没有则默认为VMA_BASE（4GB开始），之后设置VMA即可，这里映射的区域地址范围为[start, end)，左闭右开。

由于mmap调用使用了文件，因此应该为该文件的引用计数+1，防止该文件变量被回收。

代码如下：

```c
// sysfile.c
uint64
sys_mmap(void)
{
  int i;
  uint64 addr;
  int prot, flags, length, offset;
  struct file *f;
  struct proc *p = myproc();
  struct vma *a = 0;

  if(argaddr(0, &addr) || argint(1, &length) || argint(2, &prot) ||
    argint(3, &flags) || argfd(4, 0, &f) < 0 || argint(5, &offset) < 0 )
    return -1;
  
  if((flags & MAP_SHARED) && !f->writable && (prot & PROT_WRITE))
    return -1;

  for(i = 0; i < NVMA; i++){
    if(!p->vmas[i].used){
      a = &p->vmas[i];
      break;
    }
  }

  if(a == 0) return -1;

  uint64 maxend = VMA_BASE;
  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used && p->vmas[i].end > maxend)
      maxend = p->vmas[i].end;
  }
  
  a->used = 1;
  a->start = maxend;
  a->end = PGROUNDUP(a->start + length);
  a->addr = a->start;
  a->length = a->end - a->start;
  a->f = f;
  a->offset = offset;
  a->permissions = prot;
  a->flags = flags;

  filedup(f);

  return a->start;
}
```

3. 实现mmap缺页中断处理

上面的mmap系统调用只是为进程标记了映射区域，实际上并未映射物理内存，这里仍然是lazy分配的思想，等到进程实际访问映射区域时，产生缺页中断，再为所缺的虚拟页分配物理页，通过页表建立映射，并读取文件到物理页中，中断返回后进程再次访问该地址可正常访问该文件的映射。

读取文件时的偏移应为该文件在vma中起始偏移 + 该页与vma起始页的偏移。

代码如下所示：

```c
// vm.c
// Mmap pages does not exist, page fault will occur.
// Alloc a physical page and read the file to it.
int 
mmap_pgfault(uint64 stval, struct proc *p)
{
  stval = PGROUNDDOWN(stval);

  struct vma *a = 0;
  // Which vma?
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used && stval >= p->vmas[i].start && stval < p->vmas[i].end){
      a = &p->vmas[i];
      break;
    }
  }

  if(a == 0) return -1;

  char *pa = kalloc();
  if(pa == 0) return -1;
  memset(pa, 0, PGSIZE);

  int perm = PTE_U;
  if(a->permissions & PROT_READ)
    perm |= PTE_R;
  if(a->permissions & PROT_WRITE)
    perm |= PTE_W;
  if(mappages(p->pagetable, PGROUNDDOWN(stval), PGSIZE, (uint64)pa, perm) != 0)
    return -1;
  
  uint64 off = stval - a->start + a->offset;
  ilock(a->f->ip);
  if(readi(a->f->ip, 0, (uint64)pa, off, PGSIZE) <= 0){
    iunlock(a->f->ip);
    return -1;
  }
  iunlock(a->f->ip);

  return 0;
}

// trap.c
  } else if (r_scause() == 13 || r_scause() == 15) {
    uint64 stval = r_stval();
    if(mmap_pgfault(stval, p) != 0){
      printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    }

  } else if((which_dev = devintr()) != 0){
    // ok
  }
```

4. 实现munmap

`munmap()`取消映射区域的一部分，首先要找到所属的vma，取消的映射区域分为三种情况：取消vma起始地址的一段映射、取消以vma结束地址结束的一段映射、取消整个vma映射，根据三种情况得出所取消映射区域的起始地址（unstart）与长度（unlen），并更新取消一段区域后的vma。

取消映射区域的起始地址需要按页对其，长度也需要是页大小的整数倍。

如果该vma的标志为MAP_SHARED时，应该把所取消的区域的修改写回到文件中去，这里为了简单不考虑页表中的dirty位，直接写回文件；如果取消映射的页还没有通过`mmap_pgfault()`映射物理页，也就代表着没有访问修改，则不用写回。

如果取消了vma整段区域的映射，代表着该文件映射的取消，应该减少该文件的引用计数。

代码如下：

```c
// vm.c
// Whether the virtual address is mapped.
int
vm_exists(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  return (pte = walk(pagetable, va, 0)) != 0 && (*pte & PTE_V) != 0;
}

//Find the VMA for the address range and unmap the specified pages.
int
munmap(uint64 addr, int length)
{
  struct proc *p = myproc();
  struct vma *a = 0;
  addr = PGROUNDDOWN(addr);

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used && addr >= p->vmas[i].start && addr < p->vmas[i].end){
      a = &p->vmas[i];
      break;
    }
  }

  if (a == 0) return -1;

  uint64 unstart, unlen;
  uint64 start = a->start, offset = a->offset, orilen = a->length;

  if(addr == a->start){
    // Unmap at the start
    unstart = addr;
    unlen = PGROUNDUP(length) < a->length ? PGROUNDUP(length) : a->length;

    a->start = unstart + unlen; 
    a->length = a->end - a->start;
    a->offset = a->offset + unlen;
  } else if(addr + length >= a->end){
    // Unmap at the end
    unstart = addr;
    unlen = a->end - unstart;

    a->end = unstart;
    a->length = a->end - a->start;
  } else{
    // Unmap the whole region
    unstart = a->start;
    unlen = a->end - a->start;
  }
  
  for(int i = 0; i < unlen / PGSIZE; i++){
    uint64 va = unstart + i * PGSIZE;
    // May not be alloced due to lazy alloc through page fault.
    if(vm_exists(p->pagetable, va)){
      if(a->flags & MAP_SHARED){
        munmap_writeback(va, PGSIZE, start, offset, a);
      }

      uvmunmap(p->pagetable, va, 1, 1);
    }
  }

  if(unlen == orilen){
    fileclose(a->f);
    a->used = 0;
  }
  
  return 0;
}

// sysfile.c
uint64
sys_munmap(void)
{
  uint64 addr;
  int length;

  if(argaddr(0, &addr) || argint(1, &length))
    return -1;
  
  return munmap(addr, length);
}
```

5. 实现munmap_writeback

`munmap_writeback()`用于取消映射时将修改的映射页写回文件，写回时应注意写文件的起始偏移，与读文件类似，写偏移为该文件在vma中起始偏移 + 该页与vma起始页的偏移，此外该文件剩余大小不足一页大小时，应该按照实际剩余大小来写。

写文件时和`filewrite()`类似，将写操作打包为多次日志事务来写。

代码如下：

```c
// vm.c
// If an unmapped page has been modified and the file is mapped MAP_SHARED, 
// write the page back to the file. 
int
munmap_writeback(uint64 unstart, uint64 unlen, uint64 start, uint64 offset, struct vma *a)
{
  struct file *f = a->f;
  uint off = unstart - start + offset;
  uint size;

  ilock(f->ip);
  size = f->ip->size;
  iunlock(f->ip);

  if(off >= size) return -1;

  uint n = unlen < size - off ? unlen : size - off;

  int r, ret = 0;
  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int i = 0;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    begin_op();
    ilock(f->ip);
    r = writei(f->ip, 1, unstart, off + i, n1);
    iunlock(f->ip);
    end_op();

    if(r != n1){
      // error from writei
      break;
    }
    i += r;
  }
  ret = (i == n ? n : -1);

  return ret;
}
```

6. 修改exit

进程退出时应该取消所有的mmap映射，直接调用`munmap()`函数取消每一个使用中的vma的映射即可，代码如下：

```c
// proc.c/exit
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      munmap(p->vmas[i].start, p->vmas[i].length);
      p->vmas[i].used = 0;
    }
  }
```

7. 修改fork

当调用`fork()`时，子进程也需要复制父进程的映射，直接复制父进程的vma即可，不过需要为文件的引用计数+1，这里实现时不考虑父进程与子进程共享相同的物理页，如果共享的话，实现和COW fork类似，代码如下：

```c
// proc.c
// Copy vmas of parent proc for mmap
// Need to increase file reference count
void
fork_mmap(struct proc *np, struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      np->vmas[i] = p->vmas[i];
      filedup(np->vmas[i].f);
    }
  }
}

// proc.c/fork
safestrcpy(np->name, p->name, sizeof(p->name));

fork_mmap(np, p);

pid = np->pid;
```

#### 实验测试

![image-20210708130504265](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210708130511.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/mmap