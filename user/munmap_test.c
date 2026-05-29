// user/munmap_test.c
#include "../kernel/types.h"
#include "../user/user.h"
#include "../kernel/fcntl.h"
#include "user.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2
#define PGSIZE 4096

int
main(void)
{
  int after_fault, after;
  char *p;

  p = (char*)mmap(0x5000, PGSIZE * 2,
                  PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS,
                  -1, 0);

  printf("mmap addr = %p\n", p);
  printf("freemem after mmap lazy = %d\n", freemem());

  p[0] = 'A';

  after_fault = freemem();
  printf("freemem after first fault = %d\n", after_fault);

  if(munmap((uint64)p) != 1){
    printf("munmap failed\n");
    exit(1);
  }

  after = freemem();
  printf("freemem after munmap = %d\n", after);

  if(after == after_fault + 1)
    printf("munmap lazy partial test OK\n");
  else
    printf("check freemem: after_fault=%d after=%d\n", after_fault, after);

  exit(0);
}