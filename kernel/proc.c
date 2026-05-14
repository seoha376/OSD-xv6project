#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"


struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern uint ticks;
extern struct spinlock tickslock;

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;
struct mmap_area mmap_areas[MAXMMAP];
static void mmap_rollback(struct proc *p, uint64 start, uint64 end);



uint64 calculate_vdeadline(struct proc *p);
int calculate_eligibility(struct proc *p);

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void) // 자식 프로세스의 구조 생성.
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->nice = 20;
  p->runtime = 0; // initialize. set default value.
  p->vruntime = 0;
  p->vdeadline = 0;
  p->timeslice = 5;
  p->is_eligible = 1;


  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    mmap_cleanup(p);
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// 프로세스 p의 mmap_area 배열에서
// 가상주소 va를 포함하는 mmap 영역을 찾는다.
struct mmap_area*
find_mmap_area(struct proc *p, uint64 va)
{
  for(int i = 0; i < MAXMMAP; i++){
    struct mmap_area *ma = &mmap_areas[i];

    // 빈 슬롯은 건너뛴다.
    // length == 0이면 active mapping이 아니라고 본다.
    if(ma->length == 0)
      continue;

    // 혹시 전역 mmap_area처럼 쓰는 코드와 섞여 있어도 안전하게
    // 현재 프로세스 소유 mapping만 검사한다.
    // if(ma->p != p)
    //   continue;

    // va가 [addr, addr + length) 범위 안에 있으면 해당 mapping이다.
    if(ma->addr <= va && va < ma->addr + ma->length)
      return ma;
  }

  return 0;
}

// lazy mmap 영역에서 page fault가 났을 때
// page validation(정상주소인지 여부 확인)을 진행한 뒤
// fault 난 가상주소 한 페이지를 실제 물리 페이지에 연결한다.
int
mmap_handle_pagefault(struct proc *p, uint64 fault_va, uint64 scause)
{
  struct mmap_area *ma;
  uint64 va;
  char *mem;
  int perm;
  uint64 page_offset;
  int n;

  // fault_va는 페이지 중간 주소일 수 있으므로
  // 실제 매핑할 가상주소는 page boundary로 내린다.
  va = PGROUNDDOWN(fault_va);

  // fault 난 주소가 현재 프로세스의 mmap 영역 안에 있는지 찾는다.
  ma = find_mmap_area(p, va);
  if(ma == 0)
    return -1;
  // read page fault: scause == 13
  // PROT_READ 없는 영역을 읽으려 하면 실패.
  if(scause == 13){
    if((ma->prot & PROT_READ) == 0)
      return -1;
  }

  // write page fault: scause == 15
  // PROT_WRITE 없는 영역에 쓰려 하면 실패.
  else if(scause == 15){
    if((ma->prot & PROT_WRITE) == 0)
      return -1;
  }

  // 그 외 fault 원인은 이 함수가 처리하지 않는다.
  else {
    return -1;
  }

  // PTE 권한 구성.
  // user page이므로 PTE_U는 반드시 필요하다.
  perm = PTE_U;

  // mmap prot에 따라 읽기 권한 부여.
  if(ma->prot & PROT_READ)
    perm |= PTE_R;

  // mmap prot에 따라 쓰기 권한 부여.
  if(ma->prot & PROT_WRITE)
    perm |= PTE_W;

  // lazy mmap의 핵심:
  // fault 난 페이지 딱 한 장만 할당한다.
  mem = kalloc();
  if(mem == 0)
    return -1;

  // anonymous mmap은 zero-filled page여야 한다.
  // file-backed mmap도 먼저 0으로 채우면
  // 파일에서 덜 읽힌 나머지 부분이 안전하게 0으로 남는다.
  memset(mem, 0, PGSIZE);

  // MAP_ANONYMOUS가 아니면 file-backed mmap이다.
  if((ma->flags & MAP_ANONYMOUS) == 0){
    // file-backed인데 file 포인터가 없으면 잘못된 mapping.
    if(ma->f == 0){
      kfree(mem);
      return -1;
    }

    // mmap 영역 내부에서 fault 난 page의 위치.
    // 예: mapping 시작이 0x40000000이고 fault page가 0x40001000이면 4096.
    page_offset = va - ma->addr;

    // file offset + mapping 내부 offset 위치에서 한 페이지 읽는다.
    ilock(ma->f->ip);
    n = readi(ma->f->ip, 0, (uint64)mem, ma->offset + page_offset, PGSIZE);
    iunlock(ma->f->ip);

    // readi 실패 시 할당한 page를 반환하고 실패 처리.
    if(n < 0){
      kfree(mem);
      return -1;
    }
  }

  // 가상주소 va를 방금 할당한 물리 페이지 mem에 연결한다.
  // 실패하면 반드시 kfree 해서 누수를 막는다.
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0){
    kfree(mem);
    return -1;
  }

  return 0;
}


static void
mmap_rollback(struct proc *p, uint64 start, uint64 end)
{
  for(uint64 a = start; a < end; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);

    if(pte && (*pte & PTE_V)){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
      uvmunmap(p->pagetable, a, 1, 0);
    }
  }
}

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
    struct proc *p = myproc();
    


    // addr page alignment 실패
    if(addr % PGSIZE != 0)
    return 0;
    // length 실패
    if(length <= 0 || length % PGSIZE != 0)
      return 0;
    // prot 실패
    if(prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
      return 0;

    // 실제 주소 계산
    uint64 va = MMAPBASE + addr;
    uint64 end = va + length;




    // 파일 매핑 실패
    struct file *f = 0;
    if(flags & MAP_ANONYMOUS){
    if(fd != -1)
      return 0;
    } else {
      if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0)
        return 0;

      if(p->ofile[fd] == 0)
        return 0;
      f = p->ofile[fd];

      if((prot & PROT_READ) && !f->readable)
        return 0;
      if((prot & PROT_WRITE) && !f->writable)
        return 0;
    }

    // 겹치는 mmap 영역 실패
    for(int i = 0; i < MAXMMAP; i++){
      if(mmap_areas[i].p == p){
        uint64 a = mmap_areas[i].addr;
        uint64 b = a + mmap_areas[i].length;

        if(!(end <= a || va >= b))
          return 0;
      }
    }



    // 1. 빈 mmap 슬롯 찾기
    int slot = -1;

    for(int i = 0; i < MAXMMAP; i++){
        if(mmap_areas[i].p == 0){
            slot = i;
            break;
        }
    }

    // 빈 슬롯 없음 실패 처리
    if(slot < 0)
      return 0;

    // 2. metadata 저장
    mmap_areas[slot].addr = va;
    mmap_areas[slot].length = length;
    mmap_areas[slot].offset = offset;
    mmap_areas[slot].prot = prot;
    mmap_areas[slot].flags = flags;
    mmap_areas[slot].p = p;

    // file-backed면 파일 저장
    if(!(flags & MAP_ANONYMOUS)){
        mmap_areas[slot].f = filedup(p->ofile[fd]);
    } else {
        mmap_areas[slot].f = 0;
    }

    // 3. MAP_POPULATE면 즉시 페이지 생성
    if(flags & MAP_POPULATE){
        for(uint64 a = va; a < va + length; a += PGSIZE){
            // POPULATE 중 kalloc 실패
            char *mem = kalloc();

            if(mem == 0){

                mmap_rollback(p, va, a);

                if(mmap_areas[slot].f)
                    fileclose(mmap_areas[slot].f);

                memset(&mmap_areas[slot], 0,
                       sizeof(mmap_areas[slot]));
                
                return 0;
            }

            if(mem == 0){
              // 이미 할당한 페이지 rollback 필요
              return 0;
            }

            memset(mem, 0, PGSIZE);

            // file-backed면 파일 읽기
            if(!(flags & MAP_ANONYMOUS)){

                int pageoff = a - va;

                ilock(mmap_areas[slot].f->ip);

                readi(mmap_areas[slot].f->ip,
                      0,
                      (uint64)mem,
                      offset + pageoff,
                      PGSIZE);

                iunlock(mmap_areas[slot].f->ip);
            }

            int perm = PTE_U | PTE_R;

            if(prot & PROT_WRITE)
                perm |= PTE_W;

            if(mappages(p->pagetable,
                     a,
                     PGSIZE,
                     (uint64)mem,
                     perm) < 0){
                    kfree(mem);

                    mmap_rollback(p, va, a);
                                
                    if(mmap_areas[slot].f)
                        fileclose(mmap_areas[slot].f);
                                
                    memset(&mmap_areas[slot], 0,
                           sizeof(mmap_areas[slot]));
                    
                    return 0; 
                    }
        }
    }

    // 4. 성공 시 시작 주소 반환
    return va;  
}



int
munmap(uint64 addr)
{
  struct proc *p = myproc();

  // munmap은 mmap으로 받은 시작 주소만 인자로 받아야 하므로
  // page aligned 주소가 아니면 잘못된 요청으로 처리한다.
  if(addr % PGSIZE != 0)
    return -1;

  struct mmap_area *ma = 0;

  // 현재 구현은 전역 mmap_areas[]를 사용하므로,
  // 현재 프로세스 p가 소유하고 시작 주소가 addr인 mapping을 찾는다.
  // addr이 region 중간 주소이면 실패해야 한다.
  for(int i = 0; i < MAXMMAP; i++){
    if(mmap_areas[i].p == p && mmap_areas[i].addr == addr){
      ma = &mmap_areas[i];
      break;
    }
  }

  // 해당 시작 주소를 가진 mmap region이 없으면 실패.
  if(ma == 0)
    return -1;

  // mmap region 안의 page들을 하나씩 확인한다.
  // lazy mmap에서는 아직 접근하지 않은 page가 있을 수 있으므로
  // region 전체를 무조건 uvmunmap하면 안 된다.
  for(uint64 va = ma->addr; va < ma->addr + ma->length; va += PGSIZE){
    // walk(..., 0)은 새 page table을 만들지 않고
    // 기존 PTE가 있는지만 확인한다.
    pte_t *pte = walk(p->pagetable, va, 0);

    // 아직 fault가 안 난 lazy page라면 PTE 자체가 없을 수 있다.
    // 이 경우 해제할 물리 page도 없으므로 그냥 넘어간다.
    if(pte == 0)
      continue;

    // PTE가 있어도 valid하지 않으면 실제 매핑된 page가 아니다.
    if((*pte & PTE_V) == 0)
      continue;

    // R/W/X 중 하나라도 있어야 leaf PTE이다.
    // leaf가 아닌 page-table 중간 노드는 uvmunmap 대상이 아니다.
    if((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
      continue;

    // 실제로 매핑된 leaf page만 unmap하고,
    // 마지막 인자 1로 물리 page도 kfree한다.
    uvmunmap(p->pagetable, va, 1, 1);
  }

  // file-backed mmap이었다면 mmap_area가 file reference를 들고 있으므로
  // mapping 제거 시 file reference count를 줄인다.
  // anonymous mmap이면 ma->f는 0이므로 아무것도 하지 않는다.
  if(ma->f)
    fileclose(ma->f);

  // mmap_area slot을 비워서 이후 mmap에서 재사용 가능하게 한다.
  memset(ma, 0, sizeof(*ma));

  // PDF 기준 성공 반환값은 1.
  return 1;
}

void
mmap_cleanup(struct proc *p)
{
  // 전역 mmap_areas[] 중에서 종료되는 프로세스 p가 소유한
  // 모든 mmap region을 찾아 정리한다.
  for(int i = 0; i < MAXMMAP; i++){
    struct mmap_area *ma = &mmap_areas[i];

    // 다른 프로세스의 mmap metadata는 건드리면 안 된다.
    if(ma->p != p)
      continue;

    // 이 region 안에서 실제로 fault/populate되어 매핑된 page만 해제한다.
    // 아직 lazy 상태인 page는 PTE가 없을 수 있으므로 건너뛴다.
    for(uint64 va = ma->addr; va < ma->addr + ma->length; va += PGSIZE){
      pte_t *pte = walk(p->pagetable, va, 0);

      // PTE가 존재하고, valid하며, leaf PTE인 경우만 uvmunmap한다.
      // 이렇게 해야 freewalk 전에 leaf mapping이 제거되어
      // panic: freewalk: leaf를 막을 수 있다.
      if(pte && (*pte & PTE_V) && (*pte & (PTE_R | PTE_W | PTE_X)))
        uvmunmap(p->pagetable, va, 1, 1);
    }

    // file-backed mapping이면 mmap 때 잡아둔 file reference를 반환한다.
    if(ma->f)
      fileclose(ma->f);

    // metadata slot 초기화.
    memset(ma, 0, sizeof(*ma));
  }
}


// fork/kfork()에서 uvmcopy() 성공 후 호출
// parent의 mmap_area metadata와 이미 실제 mapping된 page들을 child에 복제한다.
int
mmap_forkcopy(struct proc *parent, struct proc *child)
{
  struct mmap_area *ma;
  struct mmap_area *cma;
  uint64 va;
  pte_t *pte;
  uint64 pa;
  char *mem;
  int flags;

  // [1] 전역 mmap metadata 배열 전체 순회
  for(int i = 0; i < MAXMMAP; i++){
    ma = &mmap_areas[i];

    // [1-1] 비어 있는 slot 또는 parent 소유가 아닌 mapping은 무시
    if(ma->p != parent)
      continue;

    // [2] child용 빈 mmap_area slot 찾기
    cma = 0;
    for(int j = 0; j < MAXMMAP; j++){
      if(mmap_areas[j].p == 0){
        cma = &mmap_areas[j];
        break;
      }
    }

    // [2-1] child용 metadata slot이 없으면 실패
    if(cma == 0)
      goto bad;

    // [3] metadata 복사
    // lazy page를 위해 metadata는 반드시 복사되어야 한다.
    *cma = *ma;
    cma->p = child;

    // [3-1] file-backed mmap이면 file refcount 증가
    // parent/child가 같은 struct file을 참조하므로 filedup 필요
    if(cma->f)
      filedup(cma->f);

    // [4] mmap 영역의 각 page를 순회
    for(va = ma->addr; va < ma->addr + ma->length; va += PGSIZE){

      // [4-1] parent pagetable에서 해당 VA의 PTE 확인
      pte = walk(parent->pagetable, va, 0);

      // [4-2] 아직 lazy 상태인 page
      // PTE가 없거나 valid하지 않으면 child에도 page를 만들지 않는다.
      // child는 나중에 접근 시 page fault handler가 metadata를 보고 처리한다.
      if(pte == 0)
        continue;

      if((*pte & PTE_V) == 0)
        continue;

      // [4-3] leaf PTE가 아니면 실제 physical page mapping이 아님
      if((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
        continue;

      // [5] 이미 fault/populate되어 실제 page가 있는 경우
      // child는 parent physical page를 공유하지 않고 새 page를 받아야 한다.
      pa = PTE2PA(*pte);

      mem = kalloc();
      if(mem == 0)
        goto bad;

      // [5-1] parent page 내용을 child page로 복사
      memmove(mem, (char*)pa, PGSIZE);

      // [5-2] parent PTE permission을 child에도 동일하게 적용
      flags = PTE_FLAGS(*pte);

      // [5-3] child pagetable에 같은 VA로 mapping
      if(mappages(child->pagetable, va, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto bad;
      }
    }
  }

  return 0;


// [6] 실패 시 정리
// child에 복제된 mmap metadata와 이미 mapping된 mmap page들을 제거한다.
bad:
  mmap_cleanup(child);
  return -1;
}











// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}


int weight(int nice){
  switch(nice){
    case 0: return 88761;
    case 5: return 29154;
    case 10: return 9548;
    case 15: return 3121;
    case 20: return 1024;
    case 25: return 335;
    case 30: return 110;
    case 35: return 36;
    default: return 1024;
  }
}

uint64 
calculate_vdeadline(struct proc *p) { // input : by ai
  return p->vruntime + ((uint64)p->timeslice * 1024*1000)/weight(p->nice);
}


// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void) 
// 자식 프로세스를 만듦. 자식은 부모 페이지를 그대로 쓰지 않음.
// 1. 부모 페이지를 찾는다.
// 2. 새로운 자식 페이지를 할당한다.
// 3. 부모의 메모리 내용을 복붙한다.
// 4. 새로운 mapping을 만든다.
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc(); // take parent's value

  // Allocate process.
  if((np = allocproc()) == 0){ // create child
    return -1;
  }

  // parent의 일반 user memory 복사
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  // [추가] mmap 영역 복사
  if(mmap_forkcopy(p, np) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  pid = np->pid;


  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);


  // inherit parent parameter
  np -> nice = p -> nice;
  np -> vruntime = p -> vruntime; 
  // inherit parent parameter
  np -> runtime = 0;
  np -> timeslice = 5;
  np -> vdeadline = calculate_vdeadline(np);
  np->state = RUNNABLE;

  release(&np->lock);

  // uint64 min_vruntime, sum_w, sum_numerator;
  // cal_runqueue_stats(&min_vruntime, &sum_w, &sum_numerator);
  // np -> is_eligible = is_eligible_proc(np, min_vruntime, sum_w, sum_numerator); // recalculate eligibility

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}



void
cal_runqueue_stats(uint64 *min_vruntime, uint64 *sum_w, uint64 *sum_numerator){
  struct proc *p;
  int first = 1;

  *min_vruntime = 0;
  *sum_w = 0;
  *sum_numerator = 0;


  // 1) what is minimum of vruntime?
  for(p=proc; p< &proc[NPROC]; p++){
    acquire(&p->lock);
    if (p->state!= RUNNABLE && p->state != RUNNING){
      release(&p->lock);
      continue;
    }

    if (first || *min_vruntime > p->vruntime){
      *min_vruntime = p->vruntime;
      first = 0;
    }
    release(&p->lock);
  }


  if(first)
    return;




  // 2) sigma caculation 
  for (p=proc; p< &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->state != RUNNABLE && p->state != RUNNING){
      release(&p->lock);
      continue;
    }
     
    
    uint64 w = weight(p->nice);
    *sum_w += w;
    *sum_numerator += w*(p->vruntime - *min_vruntime);
    
    release(&p->lock);
    }
  }


int
is_eligible_proc(struct proc *p, uint64 min_vruntime, uint64 sum_w, uint64 sum_numerator){
    if(sum_w==0)
      return 0;

    if(p->state != RUNNABLE && p->state != RUNNING)
      return 0;

    // check the lag is positive (if the process is eligible)
    return sum_numerator>=(p->vruntime - min_vruntime)*sum_w;
  }




// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct proc *best;
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
    uint64 min_vruntime = 0;
    uint64 sum_w = 0;
    uint64 sum_numerator = 0;

    cal_runqueue_stats(&min_vruntime, &sum_w, &sum_numerator);

    best = 0;

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);

      if(p->state != RUNNABLE){
        release(&p->lock);
        continue;
      }
      
      p->is_eligible = is_eligible_proc(p, min_vruntime, sum_w, sum_numerator);

      if(p->is_eligible == 0){
        release(&p->lock); // save p->lock into best 
        continue;
      }
        
      
      if(best == 0){
        best = p;
        continue;
      }
      
      if(p->vdeadline < best->vdeadline){
        release(&best->lock);
        best = p; // new best->lock maintained
      } else {
        release(&p->lock);
      }
    }
      

      if(best){
        if(best->state == RUNNABLE) {
          best->state = RUNNING;
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          c->proc = best;

          swtch(&c->context, &best->context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          found = 1;
        }
        release(&best->lock);
      }
      if(found == 0) {
        // nothing to run; stop running on this core until an interrupt.
        asm volatile("wfi");
    }
  }
}
// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;
  uint64 min_vruntime, sum_w, sum_numerator;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;

        p->timeslice = 5;
        p->vdeadline = calculate_vdeadline(p);

        p->is_eligible = 1;
      }
      release(&p->lock);
    }
  }


  cal_runqueue_stats(&min_vruntime, &sum_w, &sum_numerator);

  for(p=proc; p< &proc[NPROC];p++){
    acquire(&p->lock);
    p->is_eligible = is_eligible_proc(p, min_vruntime, sum_w, sum_numerator);
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
int
getnice(int pid)
{
  struct proc *p; // ai was used(gpt or gemini)

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      int n = p->nice;
      release(&p->lock);
      return n;
    }
    release(&p->lock);
  }
  return -1;
}


int
setnice(int pid, int nice)
{
  struct proc *p;
  int found = 0;
  uint64 min_vruntime, sum_w, sum_numerator;

  if(nice < 0)
    nice = 0;
  if(nice > 39)
    nice = 39;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->nice = nice;
      p->vdeadline = calculate_vdeadline(p);
      release(&p->lock);
      found = 1;
      break;
    }
    release(&p->lock);
  }
  if(!found)
    return -1;


  cal_runqueue_stats(&min_vruntime, &sum_w, &sum_numerator);

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    p->is_eligible = is_eligible_proc(p, min_vruntime, sum_w, sum_numerator);
    release(&p->lock);
  }
  
  return 0;
}

static void
print_spaces(int n)
{
  while(n-- > 0)
    printf(" ");
}

static int
strlen2(const char *s)
{
  int n = 0;
  while(s[n] != '\0')
    n++;
  return n;
}

static int
digits(uint64 x)
{
  int n = 1;
  while(x >= 10){
    x /= 10;
    n++;
  }
  return n;
}

static void
print_str_field(char *s, int width)
{
  int len = strlen2(s);
  printf("%s", s);
  if(len < width)
    print_spaces(width - len);
}

static void
print_int_field(uint64 x, int width)
{
  int len = digits(x);
  printf("%lu", x);
  if(len < width)
    print_spaces(width - len);
}



void
ps(int pid)
{
  struct proc *p;
  char *states[] = { // ai was used(gpt or gemini)
    [UNUSED]   = "UNUSED",
    [USED]     = "USED",
    [SLEEPING] = "SLEEPING",
    [RUNNABLE] = "RUNNABLE",
    [RUNNING]  = "RUNNING",
    [ZOMBIE]   = "ZOMBIE"
  };

  uint total_ticks_snapshot;
  acquire(&tickslock);
  total_ticks_snapshot = ticks;
  release(&tickslock);


  // formatting used by ai
  print_str_field("name", 12); 
  print_str_field("pid", 6);
  print_str_field("state", 12);
  print_str_field("nice", 6);
  print_str_field("runtime/w", 12);
  print_str_field("runtime", 10);
  print_str_field("vruntime", 10);
  print_str_field("vdeadline", 11);
  print_str_field("eligible", 10);
  print_str_field("total_tick", 10);
  printf("\n");

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->state == UNUSED){
      release(&p->lock);
      continue;
    }

    if(pid != 0 && p->pid != pid){
      release(&p->lock);
      continue;
    }


    int state = p->state;
    int nice = p->nice;
    int proc_pid = p->pid;

    uint64 runtime = p->runtime;
    uint64 vruntime = p->vruntime;
    uint64 vdeadline = p->vdeadline;
    uint64 w = weight(nice);

    char name[16];
    safestrcpy(name, p->name, sizeof(name));

    int is_eligible = p->is_eligible;

    release(&p->lock);



    uint64 runtime_mt   = runtime;
    uint64 vruntime_mt  = vruntime;
    uint64 vdeadline_mt = vdeadline;
    uint64 total_tick = (uint64)total_ticks_snapshot;


    uint64 runtime_per_weight = 0;
    if(w != 0)
      runtime_per_weight = (runtime * 1000) / w;

    

    print_str_field(name, 12);
    print_int_field(proc_pid, 6);
    print_str_field(states[state], 12);
    print_int_field(nice, 6);
    print_int_field(runtime_per_weight, 12);
    print_int_field(runtime_mt, 10);
    print_int_field(vruntime_mt, 10);
    print_int_field(vdeadline_mt, 11);
    print_int_field(is_eligible, 10);
    print_int_field(total_tick, 10);
    printf("\n");
  }  
}

int
waitpid(int pid)
{
  struct proc *np;
  int havekids;
  struct proc *p = myproc(); 
  

  acquire(&wait_lock); 
  
  for(;;){ 
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p && np->pid == pid){
        acquire(&np->lock); // ai was used(gpt or gemini)
        havekids = 1;
        if(np->state == ZOMBIE){
          
          freeproc(np); // ai was used(gpt or gemini)
          release(&np->lock); // ai was used(gpt or gemini)
          release(&wait_lock);
          return 0;
        }
        release(&np->lock);
      }
    }

    
    if(!havekids || p->killed){ // ai was used(gpt or gemini)
      release(&wait_lock);
      return -1; //fail -> return -1
    }

    
    sleep(p, &wait_lock); // ai was used(gpt or gemini)
  }
}