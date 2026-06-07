// PA4 (Project 4) - Page Replacement: swap.c (SKELETON)
//
// This file ships with helpers already implemented for you:
//   * swapread / swapwrite      : page-granular disk I/O against the swap
//                                  area at the tail of fs.img
//                                  (disk blocks [SWAPBASE, SWAPBASE+SWAPMAX)).
//   * swapstat                   : exposes how many disk blocks have been
//                                  read/written for swap I/O since boot.
//   * swap_alloc_slot /
//     swap_free_slot             : reserve and release page-sized slots in
//                                  the swap-slot bitmap.
//   * swapinit                   : called once at boot to set everything up.
//
// You will implement:
//   * swap_out                   : pick a victim via the clock algorithm,
//                                  write it to the swap area, rewrite the
//                                  owning PTE, return the freed frame.
//   * swap_in                    : page-fault handler entry; restore a
//                                  swapped-out page from the swap area.
//
// One swap "slot" holds exactly one PGSIZE page; each slot occupies
// (PGSIZE / BSIZE) = 4 consecutive disk blocks.  The bitmap therefore has
// (SWAPMAX / 4) bits = 7000 bits, which fits in one page.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"     // BSIZE
#include "buf.h"    // struct buf (data field accessed directly)

#define BLKS_PER_PAGE   (PGSIZE / BSIZE)             // 4
#define NSWAPSLOTS      (SWAPMAX / BLKS_PER_PAGE)    // total page-sized slots

// I/O statistics (disk blocks, not pages).
struct {
  struct spinlock lock;
  int nr_sectors_read;
  int nr_sectors_write;
} swapstats;

// Swap-slot bitmap.  bitmap[i] == 1  =>  slot i is occupied.
// Allocated once at boot from a kalloc'd page.
struct {
  struct spinlock lock;
  uchar          *bits;   // length: NSWAPSLOTS bits, stored in a single page
  uint            next;   // hint for next allocation (round-robin)
} swapmap;

void
swapinit(void)
{
  initlock(&swapstats.lock, "swapstats");
  swapstats.nr_sectors_read  = 0;
  swapstats.nr_sectors_write = 0;

  initlock(&swapmap.lock, "swapmap");
  swapmap.bits = (uchar *)kalloc();
  if(swapmap.bits == 0)
    panic("swapinit: kalloc bitmap");
  memset(swapmap.bits, 0, PGSIZE);
  swapmap.next = 0;
}

// Allocate a free swap slot.  Returns -1 if the swap is full.
int
swap_alloc_slot(void)
{
  acquire(&swapmap.lock);
  for(uint tries = 0; tries < NSWAPSLOTS; tries++){
    uint i = (swapmap.next + tries) % NSWAPSLOTS;
    uint byte = i >> 3;
    uchar mask = 1 << (i & 7);
    if((swapmap.bits[byte] & mask) == 0){
      swapmap.bits[byte] |= mask;
      swapmap.next = (i + 1) % NSWAPSLOTS;
      release(&swapmap.lock);
      return (int)i;
    }
  }
  release(&swapmap.lock);
  return -1;
}

// Mark a swap slot as free.
void
swap_free_slot(uint slot)
{
  if(slot >= NSWAPSLOTS)
    panic("swap_free_slot: bad slot");
  acquire(&swapmap.lock);
  uint byte = slot >> 3;
  uchar mask = 1 << (slot & 7);
  if((swapmap.bits[byte] & mask) == 0){
    release(&swapmap.lock);
    panic("swap_free_slot: double-free");
  }
  swapmap.bits[byte] &= ~mask;
  release(&swapmap.lock);
}

// Read one page (PGSIZE bytes) starting at physical address `ptr` from
// swap slot `blkno`.  This is the slot index, not a raw disk block number.
//
// Internally, 4 consecutive disk blocks are read into the buffer cache
// and copied out.  Statistics are bumped by BLKS_PER_PAGE.
void
swapread(uint64 ptr, int blkno)
{
  if(blkno < 0 || blkno >= NSWAPSLOTS)
    panic("swapread: bad slot");

  uint disk_blk = SWAPBASE + (uint)blkno * BLKS_PER_PAGE;
  char *dst = (char *)ptr;

  for(int i = 0; i < BLKS_PER_PAGE; i++){
    struct buf *b = bread(ROOTDEV, disk_blk + i);
    memmove(dst + i * BSIZE, b->data, BSIZE);
    brelse(b);
  }

  acquire(&swapstats.lock);
  swapstats.nr_sectors_read += BLKS_PER_PAGE;
  release(&swapstats.lock);
}

// Write one page (PGSIZE bytes) starting at physical address `ptr` into
// swap slot `blkno`.  Counterpart of swapread.
void
swapwrite(uint64 ptr, int blkno)
{
  if(blkno < 0 || blkno >= NSWAPSLOTS)
    panic("swapwrite: bad slot");

  uint disk_blk = SWAPBASE + (uint)blkno * BLKS_PER_PAGE;
  char *src = (char *)ptr;

  for(int i = 0; i < BLKS_PER_PAGE; i++){
    struct buf *b = bread(ROOTDEV, disk_blk + i);
    memmove(b->data, src + i * BSIZE, BSIZE);
    bwrite(b);
    brelse(b);
  }

  acquire(&swapstats.lock);
  swapstats.nr_sectors_write += BLKS_PER_PAGE;
  release(&swapstats.lock);
}

// Expose the running counters to user space.  Both arguments are
// kernel-side pointers; the caller (sys_swapstat) does the copyout.
void
swapstat(int *nr_sectors_read, int *nr_sectors_write)
{
  acquire(&swapstats.lock);
  if(nr_sectors_read)  *nr_sectors_read  = swapstats.nr_sectors_read;
  if(nr_sectors_write) *nr_sectors_write = swapstats.nr_sectors_write;
  release(&swapstats.lock);
}

// ============================================================================
// TODO: students implement everything below this line.
// ============================================================================

//
// swap_out:
//   Free a physical frame by evicting one of the user-mapped pages.
//
// Steps (suggested):
//   1. Ask the LRU subsystem for a victim:
//        uint64 pa = lru_select_victim(&pt, &va);
//      lru_select_victim returns the page's physical address and unlinks
//      it from the LRU list.  pt and va identify the owning PTE.
//      If the LRU is empty it returns 0.
//
//   2. Reserve a free swap slot with swap_alloc_slot().  If that fails
//      (-1), the swap area is full -- you must restore the victim to the
//      LRU (lru_add) and signal failure to the caller (return 0).
//
//   3. Write the victim's contents to that slot with swapwrite(pa, slot).
//
//   4. Walk the owning page table to find the victim's PTE and rewrite it:
//        - Replace the PPN with the swap slot index (use SLOT2PTE).
//        - Clear PTE_V (page is no longer resident).
//        - Set PTE_S (page is swapped out -- the page-fault handler will
//          recognize this).
//        - Preserve PTE_U / PTE_R / PTE_W / PTE_X.
//
//   5. Return the freed physical frame (pa cast to void *) so that the
//      caller (kalloc) can hand it to the next allocation request.
//
// On any failure, return 0.  The caller in kalloc.c will treat that as
// "no memory available" and propagate the failure upward.
//
// Useful symbols from defs.h / riscv.h:
//   walk(pt, va, alloc=0)  -- page-table walk; returns pte_t * or 0
//   PTE_FLAGS(pte)         -- extract the lower-12-bit flag field
//   SLOT2PTE(slot)         -- encode a slot index into a PTE's PPN
//   PTE_V, PTE_S, PTE_A, PTE_U, PTE_R, PTE_W, PTE_X
//   lru_select_victim(...) -- declared in defs.h
//   lru_add(pt, va, pa)    -- re-insert a frame into the LRU (used on
//                             failure to put the page back where it was)
//
void *
swap_out(void)
{
  pagetable_t pt;
  uint64 va;


  // printf("S1\n");


  // printf("[swap_out] enter\n");
  uint64 pa = lru_select_victim(&pt, &va);
  // printf("[swap_out] victim pa=%ld va=%ld\n", pa, va);

  if(pa == 0){
    // printf("[swap_out] no victim lru=%d\n", lru_size());
    return 0;
  }

  // printf("S2 pa=%ld\n", pa);

  int slot = swap_alloc_slot();
  if(slot < 0){
    lru_add(pt, va, pa);
    return 0;
  }

  // printf("S3 slot=%d\n", slot);

  swapwrite(pa, slot);

  // printf("S4\n");

  pte_t *pte = walk(pt, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0){
    swap_free_slot(slot);
    lru_add(pt, va, pa);
    return 0;
  }

  uint flags = PTE_FLAGS(*pte);
  flags &= ~PTE_V;
  flags |= PTE_S;

  *pte = SLOT2PTE(slot) | flags;
  sfence_vma();

  return (void *)pa;


  static int cnt=0;
  cnt++;

  if(cnt % 100 == 0)
    printf("swapout=%d lru=%d\n", cnt, lru_size());
}

//
// swap_in:
//   Bring a swapped-out page back into memory.  Called from the page-fault
//   handler when usertrap() sees a fault on a PTE that has PTE_V == 0 and
//   PTE_S == 1.
//
// Arguments:
//   pt -- the faulting process's page table
//   va -- the virtual address that triggered the fault
//
// Steps (suggested):
//   1. va = PGROUNDDOWN(va) -- always operate on the page boundary.
//
//   2. Walk the page table to get the PTE.  Sanity-check that it really
//      is a swapped-out entry (PTE_V == 0 and PTE_S == 1).  If not, this
//      isn't a page we can swap in -- return -1 and let the caller treat
//      the fault as a real segfault.
//
//   3. Extract the swap slot index with PTE2SLOT(*pte).
//
//   4. Allocate a fresh physical frame with kalloc().  Note: this call
//      may *itself* recursively trigger swap_out (that's fine, by design)
//      but it can still return 0 if both RAM and swap are exhausted.
//      Return -1 in that case.
//
//   5. Read the slot's contents into the new frame with swapread(...).
//      Mark the slot as free with swap_free_slot(...).
//
//   6. Rewrite the PTE to point at the new frame:
//        - PPN field <- new physical address (use PA2PTE).
//        - Set PTE_V, clear PTE_S.
//        - Preserve PTE_U / PTE_R / PTE_W / PTE_X.
//
//   7. Re-attach the new frame to the LRU list with lru_add(...).
//
//   8. Call sfence_vma() to flush the stale TLB entry.  The faulting
//      instruction will then re-execute and succeed.
//
//   Return 0 on success.
//
int
swap_in(pagetable_t pt, uint64 va)
{

  // printf("SWAPIN va=%ld\n", va);
  va = PGROUNDDOWN(va);

  pte_t *pte = walk(pt, va, 0);
  if(pte == 0)
    return -1;

  if((*pte & PTE_V) || ((*pte & PTE_S) == 0))
    return -1;

  uint flags = PTE_FLAGS(*pte);
  uint slot = PTE2SLOT(*pte);


  // 중요: kalloc() 중 재귀 swap_out/swap_in이 이 PTE를 다시 건드리지 못하게 막음
  *pte = 0;
  sfence_vma();

  char *mem = kalloc();
  if(mem == 0){
    // 실패하면 원래 swapped PTE 복구
    *pte = SLOT2PTE(slot) | flags;
    sfence_vma();
    return -1;
  }

  swapread((uint64)mem, slot);
  swap_free_slot(slot);

  flags |= PTE_V;
  flags &= ~PTE_S;

  *pte = PA2PTE((uint64)mem) | flags;
  sfence_vma();

  lru_add(pt, va, (uint64)mem);

  return 0;
}
