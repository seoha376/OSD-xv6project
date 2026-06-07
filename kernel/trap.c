#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  // user mode에서 들어온 trap인지 확인한다.
  // kernel mode에서 usertrap으로 들어오면 심각한 오류다.
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);  //DOC: kernelvec

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      kexit(-1);

    // ecall 명령어는 4바이트이므로
    // syscall 이후 다음 명령어로 넘어가게 epc 증가.
    p->trapframe->epc += 4;

    // syscall 처리 중 interrupt 허용.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 13 || r_scause() == 15){
  // user page fault 처리
  //
  // scause == 13: load page fault, 즉 read 중 page fault
  // scause == 15: store/AMO page fault, 즉 write 중 page fault
  //
  // r_stval()에는 fault가 발생한 가상주소가 들어 있다.  
  uint64 fault_va = r_stval();
  pte_t *pte = walk(p->pagetable, PGROUNDDOWN(fault_va), 0);

  if(pte && ((*pte & PTE_V) == 0) && (*pte & PTE_S)){
    if(swap_in(p->pagetable, fault_va) == 0){
      // 성공. 아무 것도 하지 않음. fault instruction 재실행됨.
    } else {
      setkilled(p);
    }
  }

    // 1순위: mmap lazy fault 처리
  //
  // fault_va가 mmap_area 안에 있으면
  // mmap_handle_pagefault()가 한 페이지만 kalloc하고 mappages한다.
  else if(mmap_handle_pagefault(p, fault_va, r_scause()) == 0){
    // mmap lazy page fault 처리 성공
  }

  // 2순위: 기존 xv6 lazy allocation 처리
  //
  // mmap 영역이 아니면 기존 vmfault()에게 처리 기회를 준다.
  // 기존 코드의 인자 의미를 유지한다.
  else if(vmfault(p->pagetable, fault_va, (r_scause() == 13) ? 1 : 0) != 0){
    // 기존 lazy allocation page fault 처리 성공
  }

  // 둘 다 처리하지 못하면 잘못된 접근이다.
  else {
    printf("usertrap: page fault failed pid=%d va=0x%lx scause=0x%lx\n",
           p->pid, fault_va, r_scause());
  // } &&
  //           vmfault(p->pagetable, r_stval(), (r_scause() == 13)? 1 : 0) != 0) {
  //   // page fault on lazily-allocated page
  // } else {
  //   printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
  //   printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }
}

  if(killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2){
    if(p && p->state == RUNNING){
    p->runtime += 1;
    p->vruntime += 1024*1000 / weight(p->nice);
    p->timeslice -= 1;

    if(p->timeslice <= 0){
      p->timeslice = 5;
      p->vdeadline = calculate_vdeadline(p);
      yield();
    }
  }
}

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
  }


//
// set up trapframe and control registers for a return to user space
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{ 
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  if(which_dev == 2){
    struct proc *p = myproc();
    if(p && p->state == RUNNING){
        p->runtime += 1;
        p->vruntime += (1024*1000)/ weight(p->nice);
        p->timeslice -= 1;

        if(p->timeslice <= 0){
          p-> timeslice = 5;
          p->vdeadline = calculate_vdeadline(p);
          yield();
        }
    }
}

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 100000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

