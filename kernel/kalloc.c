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
} kmem;

void
kinit() // 할당 시작. 할당을 위한 freepage 준비.
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p); // 각 free fage를 kfree에 넘겨.
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa) // page를 free list에 넣고, page는 reusable하게 만들어.
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

int
freemem(void)
{
  struct run *r;
  int n = 0;

  acquire(&kmem.lock);

  r = kmem.freelist;

  while(r){
    n++;
    r = r->next;
  }

  release(&kmem.lock);

  return n;
}


static int swap_enabled = 0;

void
kalloc_enable_swap(void)
{
  swap_enabled = 1;
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) // freelist에서 물리 페이지 하나를 확보한다. 추후 mappages()로 유저 virtual address로 연결함.
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist; // 사용가능한 free physical page들의 연결리스트 : 줄여서 통칭 freelist
  
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r == 0 && swap_enabled){
  printf("K1\n");
  r = (struct run*)swap_out();
  printf("K2 %p\n", r);
}

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk, 해제된 페이지를 쓰레기 값으로 덮는다.
  
  return (void*)r;
}

uint64
meminfo(void)
{
  struct run *r;
  uint64 free_pages = 0;

  acquire(&kmem.lock); // ai was used(gpt or gemini)
  r = kmem.freelist;
  while(r){
    free_pages++;
    r = r->next;
  }
  release(&kmem.lock);

  return free_pages * PGSIZE;
}