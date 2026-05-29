#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2

int
main(void)
{
  char *p;
  p = (char*)mmap(0, 4096, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

  printf("anonymous addr = %p\n", p);

  if(p == 0)
    printf("FAIL: mmap returned 0\n");
  else if(p[0] != 0)
    printf("FAIL: anonymous page not zero-filled\n");
  else {
    p[0] = 'A';
    printf("anonymous write/read = %c\n", p[0]);
    printf("PASS: anonymous populate\n");
  }

  int fd = open("README", O_RDONLY);
  if(fd < 0){
    printf("FAIL: open README\n");
    exit(1);
  }

  char *q = (char*)mmap(4096, 4096, PROT_READ,
                        MAP_POPULATE, fd, 0);

  printf("file addr = %p\n", q);

  if(q == 0)
    printf("FAIL: file mmap returned 0\n");
  else {
    printf("README first chars: %c %c %c %c\n",
           q[0], q[1], q[2], q[3]);
    printf("PASS: file populate if chars match README\n");

  }


// lazy anonymous mmap 테스트
// MAP_POPULATE가 없으므로 mmap() 시점에는 물리 페이지가 할당되면 안 된다.
// 첫 접근 p[0]에서 page fault가 발생하고,
// kernel의 mmap_handle_pagefault()가 한 페이지만 할당해야 한다.
p = (char*)mmap(
  0x2000,                         // MMAPBASE + 0x2000에 매핑
  4096,                           // 한 페이지
  PROT_READ | PROT_WRITE,         // 읽기/쓰기 허용
  MAP_ANONYMOUS,                  // anonymous mapping
  -1,                             // anonymous이므로 fd는 -1
  0                               // anonymous이므로 offset은 0
);

if(p == 0){
  printf("lazy anonymous mmap failed\n");
  exit(1);
}

printf("lazy anonymous addr=%p\n", p);

if(p[0] != 0){
  printf("lazy anonymous not zero-filled\n");
  exit(1);
}

p[0] = 'A';

if(p[0] != 'A'){
  printf("lazy anonymous write/read failed\n");
  exit(1);
}

printf("lazy anonymous ok\n");


// lazy file-backed mmap 테스트
// MAP_POPULATE가 없으므로 mmap() 시점에는 파일을 읽거나 페이지를 할당하지 않는다.
// q[0]에 접근하는 순간 page fault가 나고,
// handler가 README 첫 페이지를 읽어와야 한다.
fd = open("README", 0);
if(fd < 0){
  printf("open README failed\n");
  exit(1);
}

q = (char*)mmap(
  0x3000,             // MMAPBASE + 0x3000에 매핑
  4096,               // 한 페이지
  PROT_READ,          // 읽기 전용
  0,                  // MAP_POPULATE 없음, MAP_ANONYMOUS 없음 → file-backed lazy
  fd,                 // README fd
  0                   // 파일 처음부터 매핑
);

if(q == 0){
  printf("lazy file mmap failed\n");
  exit(1);
}

// mmap 구현에서 filedup()을 했다면 close(fd) 이후에도 q 접근이 가능해야 한다.
close(fd);

// 여기서 첫 page fault가 발생해야 한다.
// handler가 README 내용을 읽어와야 한다.
printf("lazy file first chars: %c %c %c %c\n", q[0], q[1], q[2], q[3]);

printf("lazy file ok\n");
for(;;);
  
}