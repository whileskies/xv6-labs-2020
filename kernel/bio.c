// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

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

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

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


