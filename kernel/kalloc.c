// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  int freepg_cnt;
} kmem[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
    kmem[i].freepg_cnt = 0;
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // turn off interrupt
  push_off();

  int id = cpuid();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  kmem[id].freepg_cnt++;
  release(&kmem[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r) {
    kmem[id].freelist = r->next;
    kmem[id].freepg_cnt--;
  } else {
    // free current lock
    release(&kmem[id].lock);
    int locked_current = 0;
    int find_cpu = -1;
    for (int i = 0; i < NCPU; i++) {
      if (i == id) {
        acquire(&kmem[id].lock);
        locked_current = 1;
        continue;
      }

      acquire(&kmem[i].lock);
      if (kmem[i].freepg_cnt != 0) {
        find_cpu = i;
        break;
      } else {
        release(&kmem[i].lock);
      }

    }

    if (locked_current == 0) {
      acquire(&kmem[id].lock);
    }

    if (find_cpu != -1) {
      // printf("steal from %d cnt %d\n", find_cpu, kmem[find_cpu].freepg_cnt);
      int steal_cnt = (kmem[find_cpu].freepg_cnt + 1) / 2;
      kmem[find_cpu].freepg_cnt -= steal_cnt;

      kmem[id].freelist = kmem[find_cpu].freelist;
      r = kmem[find_cpu].freelist;
      // find the last stealed page and set it next to null
      for (int i = 0; i < steal_cnt - 1; i++) {
        r = r->next;
      }
      // printf("cpuid %d steal id %d %d %p\n", id, find_cpu, steal_cnt, r);
      kmem[find_cpu].freelist = r->next;
      r->next = 0;
      kmem[id].freepg_cnt += steal_cnt;
      // stealing is done, release the lock
      release(&kmem[find_cpu].lock);

      // do the same free procedure as above
      r = kmem[id].freelist;
      kmem[id].freelist = r->next;
      kmem[id].freepg_cnt--;
    }
  }
  release(&kmem[id].lock);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
