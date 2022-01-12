// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

int ref_count[(PHYSTOP - KERNBASE) / PGSIZE];

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  for (int i = 0; i < (PHYSTOP - KERNBASE) / PGSIZE; i++) {
    ref_count[i] = 1;
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


  r = (struct run*)pa;
  uint64 index = REFCNT_INDEX((uint64)pa);

  acquire(&kmem.lock);
  ref_count[index]--;
  if (ref_count[index] == 0) {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    uint64 index = REFCNT_INDEX((uint64)r);
    kmem.freelist = r->next;
    ref_count[index] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void
add_refcnt(uint64 pa)
{
  uint64 index = REFCNT_INDEX(pa);
  acquire(&kmem.lock);
  ref_count[index]++;
  release(&kmem.lock);
}

int
on_write(pagetable_t pagetable, uint64 va)
{
  // struct proc *p = myproc();
  // if (va >= p->sz || va < p->trapframe->sp) {
  //   return -1;
  // }
  // printf("on writing %p pid %d\n", va, p->pid);

  pte_t *pte = walk(pagetable, va, 0);
  if ((*pte & PTE_RSW) == 0) {
    return -1;
  }

  uint64 pa = PTE2PA(*pte);
  uint64 index = REFCNT_INDEX(pa);
  uint64 flag = PTE_FLAGS(*pte);
  acquire(&kmem.lock);
  if (ref_count[index] == 1) {
    // allow write
    *pte |= PTE_W;
  } else {
    struct run *r = kmem.freelist;
    if(r) {
      uint64 new_index = REFCNT_INDEX((uint64)r);
      kmem.freelist = r->next;
      ref_count[new_index] = 1;
      memmove((void *)r, (void *)pa, PGSIZE);
    } else {
      release(&kmem.lock);
      return -1;
    }
    ref_count[index]--;
    *pte = PA2PTE((uint64)r) | flag | PTE_W;
  }
  release(&kmem.lock);
  return 0;
}