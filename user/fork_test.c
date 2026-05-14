#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "../kernel/fcntl.h"
#include "../user/user.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2



// [1] anonymous + populate: fork 후 child가 parent 내용 읽는지 확인
void
test_fork_anon_populate()
{
  char *p = (char*)mmap(0, 4096,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_POPULATE,
                        -1, 0);

  p[0] = 'A';
  p[1] = 'B';

  int pid = fork();

  if(pid == 0){
    if(p[0] == 'A' && p[1] == 'B')
      printf("fork anon populate: OK\n");
    else
      printf("fork anon populate: FAIL\n");
    exit(0);
  }

  wait(0);
  munmap((uint64)p);
}
// [2] anonymous + lazy: fork 후 child에서 처음 접근해 page fault 처리되는지 확인
void
test_fork_anon_lazy()
{
  char *p = (char*)mmap(4096, 4096,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS,
                        -1, 0);

  int pid = fork();

  if(pid == 0){
    p[0] = 'L';   // child에서 lazy fault 발생해야 함

    if(p[0] == 'L')
      printf("fork anon lazy: OK\n");
    else
      printf("fork anon lazy: FAIL\n");

    exit(0);
  }

  wait(0);
  munmap((uint64)p);
}
// [3] file-backed + populate/lazy: fork 후 child가 file 내용 읽는지 확인
void
test_fork_file_mmap()
{
  int fd = open("README", 0);

  if(fd < 0){
    printf("open README failed\n");
    return;
  }

  char *p = (char*)mmap(8192, 4096,
                        PROT_READ,
                        MAP_POPULATE,
                        fd, 0);

  int pid = fork();

  if(pid == 0){
    if(p[0] != 0)
      printf("fork file mmap: OK, first char=%c\n", p[0]);
    else
      printf("fork file mmap: FAIL\n");

    exit(0);
  }

  wait(0);
  munmap((uint64)p);
  close(fd);
}


int
main(int argc, char *argv[])
{
  test_fork_anon_populate();
  test_fork_anon_lazy();
  test_fork_file_mmap();

  exit(0);
}