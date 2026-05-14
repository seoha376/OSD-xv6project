// user/mmap_more_test.c
#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "../kernel/fcntl.h"
#include "user.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2
#define PGSIZE 4096

void
test_exit_cleanup(void)
{
char *p = (char*)mmap(0x6000, PGSIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS,
                      -1, 0);
  p[0] = 'X';
  printf("exit cleanup child mapped and touched page\n");
  exit(0);
}

int
main(void)
{
  int before, after;
  char *p;

  printf("=== 1. exit cleanup test ===\n");

  int pid = fork();
  if(pid == 0){
    test_exit_cleanup();
  }

  wait(0);
  printf("exit cleanup test OK if no panic occurred\n");

  printf("=== 2. freemem sanity test ===\n");

  before = freemem();

p = (char*)mmap(0x7000, PGSIZE,
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_POPULATE,
                -1, 0);

  after = freemem();

  printf("before populate mmap = %d\n", before);
  printf("after populate mmap  = %d\n", after);

  if(after < before)
    printf("freemem decreases on populate OK\n");
  else
    printf("freemem decrease check FAILED\n");

  munmap((uint64)p);

  printf("after munmap = %d\n", freemem());

  printf("=== 3. munmap populate 2-page test ===\n");

  before = freemem();

  p = (char*)mmap(0x8000, PGSIZE * 2,
                  PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_POPULATE,
                  -1, 0);

  after = freemem();

  printf("before = %d\n", before);
  printf("after populate 2 pages = %d\n", after);

  if(before - after == 2)
    printf("populate allocated 2 data pages OK\n");
  else
    printf("check allocation diff: %d\n", before - after);

  if(munmap((uint64)p) != 1){
    printf("munmap failed\n");
    exit(1);
  }

  int final = freemem();

  printf("after munmap = %d\n", final);

  if(final == after + 2)
    printf("munmap populate 2-page test OK\n");
  else
    printf("check free diff: after=%d final=%d\n", after, final);

  exit(0);
}