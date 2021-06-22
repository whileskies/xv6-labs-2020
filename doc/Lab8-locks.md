## Lab8: locks

Lab8通过重新设计内核的一些代码，将原本粗粒度的锁更改为细粒度的锁来增加并行度。多核机器上弱并行度的一个通用的标志是具有较高的锁的争用，提高并行度通常涉及到修改数据结构和加锁策略。本实验需要重新设计xv6的memory allocator和block cache两个模块的数据结构和加锁策略，来提高并行度。

#### Memory allocator

##### 任务

在原本xv6的`kalloc()`和`kfree()`的设计中，会发生较高的锁的争用，原因在于使用了一个单一的空闲物理页链表用于分配与释放物理页，该链表使用一个锁进行保护。为了移除锁的争用，需要重新设计内存分配器来避免单一的锁和链表。基本的想法是每个CPU维护一个空闲物理页链表，相应的每个链表持有一个自己的锁。在不同CPU上去分配和释放可以是并行的，因为每个CPU将操作不同的链表。其中主要的挑战是当一个CPU的链表为空时，同时另一个CPU的链表中有空闲的物理页；这种情况时，链表为空的CPU需要窃取一些其他CPU链表上的空闲物理页；窃取可能会产生锁的争用，不过还好这将不经常发生。

##### 过程

1. 定义每个CPU一个空闲物理页链表

将原本的单链表扩展为每个CPU一个链表，并在`kinit()`函数中初始化每个锁：

```c
#define LOCK_NAME_N 6

struct {
  struct spinlock lock;
  char lock_name[LOCK_NAME_N];
  struct run *freelist;
} kmem[NCPU]

void
kinit()
{
  for(int i = 0; i < NCPU; i++) {
    snprintf(kmem[i].lock_name, LOCK_NAME_N, "kmem%d", i);
    initlock(&kmem[i].lock, kmem[i].lock_name);
  }
  freerange(end, (void*)PHYSTOP);
}
```

2. 修改`kalloc()`函数

当申请物理页时，`kalloc()`被调用，由于现在每个CPU一个链表，因此需要通过`cpuid()`函数获取到当前调用`kalloc()`线程所属CPU的编号，之后从该CPU的链表中去获取一个空闲物理页；同时要注意只有中断关闭时通过`cpuid()`获取CPU号才是安全的，否则可能由于中断，之后的代码被调度到其他CPU上去执行了。

在当前CPU链表不为空时，首先对当前CPU链表加锁，并从链表中获取到空闲物理页。由于每个CPU一个锁，因此可以使得多个CPU同时调用`kalloc()`函数并分配各自链表上的空闲物理页，提高了并行度。

在当前CPU链表为空时，依次遍历其他CPU的链表，当遇到非空的CPU链表，则窃取该链表头部的空闲物理页，窃取过程中应加上被窃取CPU链表的锁。

有个细节是在窃取之前，应先释放当前CPU链表的锁，防止之后窃取过程发生死锁，所窃取的物理页之后直接作为返回值返回了，也不需要再操作当前链表。在最初的尝试中，想法是每次从其他非空的CPU链表中窃取一半的空闲物理页到当前CPU链表，但是无奈需要同时加上两个锁，会发生死锁。

修改后的`kalloc()`函数如下所示：

```c
void *
kalloc(void)
{
  struct run *r;
  
  push_off();
  int cpu = cpuid();
  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if(r) {
    kmem[cpu].freelist = r->next;
    release(&kmem[cpu].lock);
  }else {
    release(&kmem[cpu].lock);

    for(int nextid = 0; nextid < NCPU; nextid++) {
      if(cpu != nextid) {
        acquire(&kmem[nextid].lock);
        r = kmem[nextid].freelist;
        if(r) {
          kmem[nextid].freelist = r->next;
          release(&kmem[nextid].lock);
          break;
        }
        release(&kmem[nextid].lock);
      }
    }
  }
  pop_off();
  
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
```

3. 修改`kfree()`函数

`kfree()`函数的修改相对简单，只要把被释放的物理页放到当前CPU链表的头部即可，代码如下：

```c
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu = cpuid();
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
  pop_off();
}
```

##### 运行结果

![image-20210620172413597](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210620172413.png)

#### Buffer cache

##### 任务

buffer cache位于xv6文件系统中的Disk上层、Logging下层，用于缓存磁盘块，如下图所示：

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210620172804.png" alt="image-20210620172804214" style="zoom: 50%;" />

在原本的buffer cache设计中，多个buffer块用一个双向链表来组织，并通过一个锁(bcache.lock)来避免获取和释放中的race condition，通过LRU算法来淘汰buffer。

由此可见，只有一个锁，造成了大量的锁的争用，该部分的任务仍然是修改buffer cache的数据结构和加锁策略来减少争用，提高并行度。

减少buffer cache中的争用比kalloc的更为复杂，因为buffer cache是被进程（多个CPU）共享的，不能像kalloc一样每个CPU有一个自己的空闲物理页链表，每个链表上的空白物理页都是等同的，分配时不需要考虑究竟是哪个物理页，但是buffer cache中的每个buffer却是不同的。

解决办法是使用hash表，同时每个hash bucket有一个锁。

##### 过程

1. 修改数据结构为hash表

之前的buffer采用双向链表组织，为降低锁的粒度，可以使用hash表来组织，根据块号(blockno)进行hash，将buffer分布到相应的bucket中去。每个hash bucket中的buffer同样采用双向链表组织。

为了方便，每个bucket链表有一个buf作为dummy结点，用作链表头。

每个bucket有一个锁，这样获取和分配时可直接针对某个bucket进行，实现不同bucket间的并行。

同时每个buffer维护一个ticks作为时间戳，用来实现LRU算法。

代码如下：

```c
// kernel/buf.h
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];

  uint ticks;
};

// kernel/bio.c
#define NBUCKET 13

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;

  // Hash bucket
  struct buf bucket[NBUCKET];
  struct spinlock bucket_lock[NBUCKET];
} bcache;

int
hash(uint blockno)
{
  return blockno % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.bucket_lock[i], "bcache.bucket");
  }

  for(int i = 0; i < NBUCKET; i++) {
    bcache.bucket[i].next = &bcache.bucket[i];
    bcache.bucket[i].prev = &bcache.bucket[i];
  }
  
  // Create hash table of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int i = hash(b->blockno);
    b->next = bcache.bucket[i].next;
    b->prev = &bcache.bucket[i];

    bcache.bucket[i].next->prev = b;
    bcache.bucket[i].next = b;
    b->ticks = ticks;
    initsleeplock(&b->lock, "buffer");
  }
}
```

2. 修改`bget()`函数

`bget()`函数可分为两步进行：

第一步：首先查找是否存在给定dev和blockno的buffer，如果存在，则使用此buffer并返回；

第二步：如果不存在，则寻找一个LRU未使用的buffer并返回。

这里面实现时有很多需要注意的细节：

- buf为共享变量，访问其中的变量时(如refcnt)需要加buf所在bucket的锁来互斥访问。
- `hi = hash(blockno)`，第一步应该从bucket hi中查找，如果未查找到，执行第二步，第二步从其他bucket中查找未使用buffer，并将该buffer添加到bucket hi中（如果不在同一bucket上）。

- 第二步可以使用一个大锁来使其串行化，如果不加有下面几种可能做法以及出现的问题：
  - 第一步中bucket hi的锁不释放：一个CPU持有bucket hi的锁，希望遍历bucket B并申请B的锁，同时另一个CPU持有bucket B锁，希望遍历bucket hi并申请hi的锁，则发生死锁；
  - 第一步bucket hi的锁释放，第二步使用全局的LRU查找所有的bucket中未使用的buffer，两个CPU同时寻找dev和blockno相同的buffer，第一步未找到，同时寻找到了一个未使用buffer，但这两个寻找到的buffer不同，违反了`the invariant that at most one copy of each block is cached`的要求，一个块最多只能有一个buffer，会出现`panic: freeing free block`的错误；
- 使用了大锁来使得第二步的淘汰过程串行化时，首先应在第二步开始再次寻找一遍buffer，原因是两个CPU同时寻找dev和blockno相同的buffer，第一步未找到，第一个CPU通过加锁运行第二步找到一个buffer，释放锁后第二个CPU运行第二步，此时第一个CPU已经将找到的buffer作为该dev和blockno的buffer，如果第二个CPU再次寻找一个新的buffer作为相同dev和blockno的buffer，则同样违反了`the invariant that at most one copy of each block is cached`的要求；因此在第二步应首先再次在相应的bucket hi中找一次，如果之前已经有CPU找到了该buffer，直接使用即可。
- 尽量避免同时加两把锁，这将有可能导致死锁。

看网上的实现还有方式是`bget()`根据`hi = hash(bockno)`直接从bucket hi的链表中寻找，寻找不到后通过局部的LRU只从hi的链表中去寻找未使用的buffer，这样可以移除第二步的大锁，而且实现起来还挺简单，不过似乎不符合hints中的实现方式，感觉局部LRU更容易产生`panic("bget: no buffers");`。

代码如下：

```c
// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int hi = hash(blockno); 
  
  // Find cache from bucket hi.
  acquire(&bcache.bucket_lock[hi]);
  for(b = bcache.bucket[hi].next; b != &bcache.bucket[hi]; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket_lock[hi]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // Release lock hi, avoid deadlock.
  release(&bcache.bucket_lock[hi]);

  // Steal lock makes the eviction serialized in bget.
  acquire(&bcache.lock);

  // Check again cache to maintain the invariant that at most one copy of each block is cached. 
  acquire(&bcache.bucket_lock[hi]);
  for(b = bcache.bucket[hi].next; b != &bcache.bucket[hi]; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.bucket_lock[hi]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_lock[hi]);
  
  struct buf *minb = 0;
  uint min_ticks = ~0;

  // Find Recycle the least recently used (LRU) unused buffer.
  for(int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.bucket_lock[i]);
    int find = 0;
    for(b = bcache.bucket[i].next; b != &bcache.bucket[i]; b = b->next) {
      if(b->refcnt == 0 && b->ticks < min_ticks) {
        if(minb != 0) {
          int last = hash(minb->blockno);
          if(last != i)
            release(&bcache.bucket_lock[last]);
        }
        
        min_ticks = b->ticks;
        minb = b;
        find = 1;
      }
    }

    if(!find)
      release(&bcache.bucket_lock[i]);
  }

  if(minb == 0)
    panic("bget: no buffers");
  
  int minb_i = hash(minb->blockno);

  minb->dev = dev;
  minb->blockno = blockno;
  minb->valid = 0;
  minb->refcnt = 1;

  if (minb_i != hi) {
    minb->prev->next = minb->next;
    minb->next->prev = minb->prev;
  }
  release(&bcache.bucket_lock[minb_i]);
  
  if(minb_i != hi) {
    // Move the buf from original bucket to bucket hi.
    acquire(&bcache.bucket_lock[hi]);

    minb->next = bcache.bucket[hi].next;
    minb->prev = &bcache.bucket[hi];
    bcache.bucket[hi].next->prev = minb;
    bcache.bucket[hi].next = minb;

    release(&bcache.bucket_lock[hi]);
  }

  release(&bcache.lock);

  acquiresleep(&minb->lock);

  return minb;
}
```

3. 修改`brelse()`函数

`brelse()`的修改则比较简单，直接将buffer的引用数-1，如果之后未被引用的话，更新ticks，表示为最新的未使用的buffer，用于LRU算法寻找最近最少使用的unused buffer。

代码如下：

```c
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int hi = hash(b->blockno);
  acquire(&bcache.bucket_lock[hi]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->ticks = ticks;
  }
  release(&bcache.bucket_lock[hi]);
}
```

4. 修改`bpin()`和`bunpin()`

更新buffer的引用数时，应加锁，引用数为共享变量，代码如下：

```c
void
bpin(struct buf *b) {
  int hi = hash(b->blockno);
  acquire(&bcache.bucket_lock[hi]);
  b->refcnt++;
  release(&bcache.bucket_lock[hi]);
}

void
bunpin(struct buf *b) {
  int hi = hash(b->blockno);
  acquire(&bcache.bucket_lock[hi]);
  b->refcnt--;
  release(&bcache.bucket_lock[hi]);
}
```

##### 运行结果

![image-20210622153929205](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210622153929.png)

#### 实验测试

![image-20210622161745154](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210622161745.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/lock
