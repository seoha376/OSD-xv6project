# PA4 Skeleton — Setup Guide

This README walks you through every kernel-tree change needed to make the
`swap.c` / `lru.c` skeletons compile and link, plus the user-space plumbing
the tests rely on. Read the slide deck for the conceptual material; this
file is just the mechanical checklist.

The substantive part of the project — the TODO functions inside `swap.c`
and `lru.c`, and the integration hooks in `kalloc.c`, `vm.c`, and `trap.c`
— is **not** in this file. See the slide deck.

---

## 1. Drop the files in place

```
kernel/swap.c
kernel/lru.c
```

Then open the kernel Makefile and add these two lines next to the other
`$K/*.o` entries (e.g., right after `$K/kalloc.o`):

```make
$K/swap.o\
$K/lru.o\
```

---

## 2. Headers and constants

The skeletons reference symbols that don't exist in vanilla xv6 yet. Add
them to the headers below.

### `kernel/types.h` — append the per-frame node struct

```c
struct page {
    struct page *next, *prev;
    pagetable_t  pagetable;
    uint64       vaddr;
};
```

### `kernel/riscv.h` — extend the PTE flag macros

Add alongside `PTE_V`, `PTE_R`, etc.:

```c
#define PTE_S (1L << 8)
#define SLOT2PTE(slot) PA2PTE((uint64)(slot) << 12)
#define PTE2SLOT(pte)  ((uint)(PTE2PA(pte) >> 12))
```

`PTE_S` repurposes one of the RSW (software-reserved) bits as the
"swapped out" marker. The two helper macros let you stash a swap-slot
index in the PPN field exactly like you stash a physical address.

### `kernel/memlayout.h` — swap area and tuned PHYSTOP

```c
#define SWAPBASE 2000
#define SWAPMAX  28000
#define PHYSTOP  (KERNBASE + 4*1024*1024 + 512*1024)  // 4.5 MiB; tune as needed
```

The recommended PHYSTOP is the sweet spot where the shell still boots
comfortably and the provided tests actually exercise swap. If you change
it later, expect different numbers from the test programs.

### `kernel/param.h` — expand the disk image

```c
#define FSSIZE 30000   // was 2000
```

The expansion gives us room at the tail of `fs.img` to use as a swap
area. After changing this, delete `fs.img` and rebuild so it is
regenerated at the new size.

### `kernel/defs.h` — function declarations

```c
// swap.c
void   swapinit(void);
void   swapread(uint64 ptr, int blkno);
void   swapwrite(uint64 ptr, int blkno);
void   swapstat(int *nr_sectors_read, int *nr_sectors_write);
int    swap_alloc_slot(void);
void   swap_free_slot(uint slot);
void  *swap_out(void);
int    swap_in(pagetable_t pt, uint64 va);

// lru.c
void   lruinit(void);
void   lru_add(pagetable_t pt, uint64 va, uint64 pa);
void   lru_remove(uint64 pa);
int    lru_size(void);
uint64 lru_select_victim(pagetable_t *out_pt, uint64 *out_va);
```

---

## 3. Initialize at boot

In `kernel/main.c`, after the disk subsystem is up but before user
processes run, call the two initializers. A good spot is right after
`binit()` and `iinit()`:

```c
swapinit();
lruinit();
```

Order matters: `swapinit()` calls `kalloc()` to grab a page for its
bitmap, so the kernel allocator must already be initialized (it is —
`kinit()` runs earlier in main).

---

## 4. User-space syscall plumbing (for swapstat)

The test program calls `swapstat()` from user space. Wire it up the same
way as any other xv6 syscall.

### `user/user.h` — append

```c
int swapstat(int *nr_sectors_read, int *nr_sectors_write);
```

### `user/usys.pl` — register

```perl
entry("swapstat");
```

### `kernel/syscall.h` — assign a number

```c
#define SYS_swapstat 22   // use the next available number
```

### `kernel/syscall.c` — extern + table entry

```c
extern uint64 sys_swapstat(void);
// ...
[SYS_swapstat] sys_swapstat,
```

### `kernel/sysproc.c` — implementation

```c
uint64
sys_swapstat(void)
{
    uint64 ra_addr, wa_addr;
    int r, w;

    argaddr(0, &ra_addr);
    argaddr(1, &wa_addr);
    swapstat(&r, &w);

    if(copyout(myproc()->pagetable, ra_addr, (char *)&r, sizeof(r)) < 0)
        return -1;
    if(copyout(myproc()->pagetable, wa_addr, (char *)&w, sizeof(w)) < 0)
        return -1;
    return 0;
}
```

---

## 5. Verify the skeleton builds

At this point you should be able to:

```
$ make clean
$ make qemu
```

The kernel should boot to a shell. The TODO functions are no-ops, so the
swap path is inert; running `pa4_test 1 1` will report
`swap-ins: 0, swap-outs: 0`. That's the expected starting state — your
job from here is to fill in the TODOs and wire them into `kalloc`, `vm`,
and `trap`. The slide deck covers those integration points.

If the build breaks, the most common causes are:

- Forgot to add `$K/swap.o` and `$K/lru.o` to the Makefile.
- Forgot one of the declarations in `defs.h`, so a call inside `swap.c`
  or `lru.c` is unresolved.
- Changed `FSSIZE` without deleting the old `fs.img`. Delete it and
  re-run `make qemu`.
- Missing `struct page` definition in `types.h`, so `pages[]` in `lru.c`
  has incomplete type.
