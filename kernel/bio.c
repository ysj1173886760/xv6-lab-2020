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

struct bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct spinlock eviction;
  struct buf buf[NBUF];
  struct bucket bkt[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.eviction, "bcache");
  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.bkt[i].lock, "bcache");
    // Create linked list of buffers
    bcache.bkt[i].head.prev = &bcache.bkt[i].head;
    bcache.bkt[i].head.next = &bcache.bkt[i].head;
  }

  for (int i = 0; i < NBUF; i++) {
    int idx = i % NBUCKET;
    b = &bcache.buf[i];
    b->next = bcache.bkt[idx].head.next;
    b->prev = &bcache.bkt[idx].head;
    initsleeplock(&b->lock, "buffer");
    bcache.bkt[idx].head.next->prev = b;
    bcache.bkt[idx].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int idx = blockno % NBUCKET;
  acquire(&bcache.bkt[idx].lock);
  // Is the block already cached?
  for(b = bcache.bkt[idx].head.next; b != &bcache.bkt[idx].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bkt[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  
  // printf("eviction same block here\n");
  // first check whether we have free block on the same bucket
  for(b = bcache.bkt[idx].head.prev; b != &bcache.bkt[idx].head; b = b->prev){
    if(b->refcnt == 0) {
      // nice, we have free block on the same bucket, so we don't have to migration
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.bkt[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // printf("eviction here\n");

  // if we don't, then try to steal a block from other bucket
  // first we need to release our lock to prevent dead lock
  release(&bcache.bkt[idx].lock);
  // then acquire the global evition block
  acquire(&bcache.eviction);
  struct buf *free_buf = 0;
  int locked_idx = 0;
  int free_bkt = -1;
  for (int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.bkt[i].lock);

    if (i == idx)
      locked_idx = 1;

    for(b = bcache.bkt[idx].head.prev; b != &bcache.bkt[idx].head; b = b->prev){
      if(b->refcnt == 0) {
        free_bkt = i;
        free_buf = b;
        break;
      }
    }

    if (free_buf != 0)
      break;

    if (i != idx)
      release(&bcache.bkt[i].lock);
  }

  if (locked_idx == 0)
    acquire(&bcache.bkt[locked_idx].lock);

  // now, if we found a free buffer from other bucket, we should hold 2 bucket lock
  // if we didn't find free buffer, then we only hold 1 lock
  if (free_bkt == -1) {
    // if we didn't find free buffer, one last chance for find free buffer or find the required block on current bkt
    // because before we acquire eviction lock, we released the current bkt lock. now we need to check it again
    for(b = bcache.bkt[idx].head.next; b != &bcache.bkt[idx].head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.bkt[idx].lock);
        release(&bcache.eviction);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // try to find free buffer
    for(b = bcache.bkt[idx].head.next; b != &bcache.bkt[idx].head; b = b->next){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.bkt[idx].lock);
        release(&bcache.eviction);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // otherwise, we panic
    printf("here\n");
    panic("bget: no buffers");
  } else {
    // in this circumstance, we are holding idx lock and free_bkt lock
    // i.e. we found a free buffer. but we still need to check if there has already have the buffer we need
    for(b = bcache.bkt[idx].head.next; b != &bcache.bkt[idx].head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.bkt[free_bkt].lock);
        release(&bcache.bkt[idx].lock);
        release(&bcache.eviction);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // then check do we have free buffer on the same bucket, we really don't want to migration
    for(b = bcache.bkt[idx].head.next; b != &bcache.bkt[idx].head; b = b->next){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.bkt[free_bkt].lock);
        release(&bcache.bkt[idx].lock);
        release(&bcache.eviction);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // ok then, we need to migration buffer now, at least we don't need to panic
    // remove from origin bucket
    free_buf->prev->next = free_buf->next;
    free_buf->next->prev = free_buf->prev;
    
    // insert into the idx bucket
    free_buf->next = bcache.bkt[idx].head.next;
    free_buf->prev = &bcache.bkt[idx].head;
    bcache.bkt[idx].head.next->prev = free_buf;
    bcache.bkt[idx].head.next = free_buf;

    free_buf->dev = dev;
    free_buf->blockno = blockno;
    free_buf->valid = 0;
    free_buf->refcnt = 1;

    release(&bcache.bkt[free_bkt].lock);
    release(&bcache.bkt[idx].lock);
    release(&bcache.eviction);
    acquiresleep(&free_buf->lock);

    return free_buf;
  }

  panic("bget: no buffers");
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

  int idx = b->blockno % NBUCKET;
  acquire(&bcache.bkt[idx].lock);

  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.bkt[idx].head.next;
    b->prev = &bcache.bkt[idx].head;
    bcache.bkt[idx].head.next->prev = b;
    bcache.bkt[idx].head.next = b;
  }
  
  release(&bcache.bkt[idx].lock);
}

void
bpin(struct buf *b) {
  int idx = b->blockno % NBUCKET;
  acquire(&bcache.bkt[idx].lock);
  b->refcnt++;
  release(&bcache.bkt[idx].lock);
}

void
bunpin(struct buf *b) {
  int idx = b->blockno % NBUCKET;
  acquire(&bcache.bkt[idx].lock);
  b->refcnt--;
  release(&bcache.bkt[idx].lock);
}


