// PA4 (Project 4) - LRU list of swappable user pages (SKELETON).
//
// Every physical frame currently mapped into a user page table is linked into
// a single global circular doubly-linked list ordered from least-recently-
// inserted (lru.head) to most-recently-inserted (the node just before
// lru.head).  The "clock" page-replacement algorithm walks the list from
// lru.head: if the owning PTE has PTE_A == 1, it clears the bit and moves the
// node to the tail (giving the page a second chance); otherwise the page is
// chosen as the victim.
//
// The list nodes live in the pages[] array, indexed by frame number.
// A node with pagetable == 0 means "not on the list".

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// One struct page per physical frame in [KERNBASE, PHYSTOP).
//
// Implementation note: the textbook spec says "pages[PHYSTOP / PGSIZE]", but
// in xv6-riscv that would waste ~17 MiB of BSS because PHYSTOP/PGSIZE
// includes every byte from address 0, including the large region below
// KERNBASE that is never RAM.  We size by the actual RAM range instead.
#define NPHYS_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)

struct page {
  struct page *next;
  struct page *prev;
  pagetable_t pagetable;
  uint64 vaddr;
};

struct page pages[NPHYS_PAGES];

struct {
  struct spinlock lock;
  struct page    *head;   // oldest first; victim search starts here
  int             count;
} lru;

// Convert a physical address to its slot in the pages[] array.
static inline struct page *
pa_to_page(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP)
    panic("pa_to_page: bad pa");
  return &pages[(pa - KERNBASE) / PGSIZE];
}

void
lruinit(void)
{
  initlock(&lru.lock, "lru");
  lru.head  = 0;
  lru.count = 0;
  for(int i = 0; i < NPHYS_PAGES; i++){
    pages[i].next      = 0;
    pages[i].prev      = 0;
    pages[i].pagetable = 0;
    pages[i].vaddr     = 0;
  }
}

// Current length of the LRU list (for diagnostics).
int
lru_size(void)
{
  int n;
  acquire(&lru.lock);
  n = lru.count;
  release(&lru.lock);
  return n;
}

// ============================================================================
// TODO: students implement everything below this line.
// ============================================================================

//
// lru_add:
//   Attach a freshly-mapped user frame to the LRU list.  Called from
//   mappages() whenever a PTE with PTE_U is installed, and from swap_in()
//   when a frame is brought back from disk.
//
// Steps (suggested):
//   1. Look up the struct page for this pa (pa_to_page).
//   2. Acquire lru.lock.
//   3. If the node is already on the list (pg->pagetable != 0), unlink it
//      first -- we'll re-insert at the tail to reflect freshness.  This
//      keeps the list well-formed when swap_in re-adds a page that
//      somehow lingered.
//   4. Set pg->pagetable / pg->vaddr to identify the owning mapping.
//   5. Insert pg at the tail of the circular list.  Tip: the tail is
//      lru.head->prev for a non-empty list.  Be sure to handle the empty
//      list (lru.head == 0) as a special case where pg->next = pg->prev = pg.
//   6. Increment lru.count.
//   7. Release lru.lock.
//
void
lru_add(pagetable_t pt, uint64 va, uint64 pa)
{
  // TODO: implement.
}

//
// lru_remove:
//   Detach a frame from the LRU list.  Called from uvmunmap() whenever a
//   valid user PTE is torn down, so the bookkeeping matches the actual
//   user mappings.
//
// Steps (suggested):
//   1. Look up the struct page (pa_to_page).
//   2. Acquire lru.lock.
//   3. If pg->pagetable == 0, the page wasn't on the list (e.g. swapped
//      out, or never tracked) -- just release the lock and return.
//   4. Unlink pg from the circular list:
//        - If pg is the only element, set lru.head = 0.
//        - Otherwise patch pg->prev and pg->next to skip pg, and if pg
//          was the head, advance lru.head.
//   5. Clear pg->pagetable / pg->vaddr / pg->next / pg->prev.
//   6. Decrement lru.count.
//   7. Release lru.lock.
//
void
lru_remove(uint64 pa)
{
  // TODO: implement.
}

//
// lru_select_victim:
//   The clock-algorithm core.  Walk the list from lru.head; for each
//   candidate, look at the owning PTE's PTE_A bit:
//
//     PTE_A == 1 : the page was accessed recently.  Clear PTE_A (so it
//                  becomes a candidate next time), rotate the node to
//                  the tail (advance lru.head), and continue.
//     PTE_A == 0 : pick this page as the victim.  Unlink it from the LRU
//                  and return its physical address.  Write the owning
//                  pagetable and vaddr into *out_pt / *out_va so the
//                  caller (swap_out) can find the right PTE.
//
//   Returns 0 if the LRU is empty.
//
// Termination guarantee:
//   After one full pass clears every PTE_A bit, the next pass is
//   guaranteed to find a page with PTE_A == 0.  Bound the loop at
//   2 * lru.count iterations as a safety net.
//
// Implementation notes:
//   * Use walk(pg->pagetable, pg->vaddr, 0) to obtain the PTE pointer.
//   * After clearing PTE_A, sfence_vma() so the change is visible to the
//     MMU on subsequent accesses.
//   * Defensive check: if walk() returns 0 or the PTE has PTE_V == 0,
//     the LRU node is stale; drop it (unlink + clear pagetable/vaddr)
//     and continue with the next candidate.
//   * Don't forget to release lru.lock on every exit path.
//
// Useful symbols:
//   PTE_A, PTE_V             -- riscv.h
//   PTE2PA(pte)              -- extract the physical address from a PTE
//   walk(pt, va, alloc=0)    -- page-table walk
//   sfence_vma()             -- TLB flush
//
uint64
lru_select_victim(pagetable_t *out_pt, uint64 *out_va)
{
  // TODO: implement.
  return 0;
}
