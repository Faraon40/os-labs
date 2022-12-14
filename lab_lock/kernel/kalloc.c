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
} kmem[NCPU];

void
kinit()
{
  // initlock(&kmem.lock, "kmem");
  char buf[16];
  for (int i = 0; i < NCPU; i++) {
    snprintf(buf, 16, "kmem: CPU %d", i);
    initlock(&kmem[i].lock, buf);
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

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int hartid;
  
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  hartid = cpuid();
  pop_off();

  acquire(&kmem[hartid].lock);
  r->next = kmem[hartid].freelist;
  kmem[hartid].freelist = r;
  release(&kmem[hartid].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int hartid;

  push_off();
  hartid = cpuid();
  pop_off();

  acquire(&kmem[hartid].lock);
  r = kmem[hartid].freelist;
  if(r) {
    kmem[hartid].freelist = r->next;
  }
  else {
    for (int i = 0; i < NCPU; i++) {
      if (i == hartid) {
        continue;
      }
      acquire(&kmem[i].lock);
      struct run *current = kmem[i].freelist;
      if (!current) {
        release(&kmem[i].lock);
        continue;
      }

      for (int j = 0; j < 16 * 1024; j++) {
        if (!current->next) {
          break;
        }
        current = current->next;
      }

      r = kmem[i].freelist;
      kmem[i].freelist = current->next;
      current->next = 0;
      if (r) {
        kmem[hartid].freelist = r->next;
      }
      release(&kmem[i].lock);
      break;
    }
  }
  release(&kmem[hartid].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
