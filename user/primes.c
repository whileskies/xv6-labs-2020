#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
filter(int lpipe[])
{
  int rpipe[2];
  pipe(rpipe);

  int primes[50];
  int cnt = 0;
  char buf[1];

  while ((read(lpipe[0], buf, sizeof(buf))) != 0) {
    primes[cnt++] = buf[0];
  }
  close(lpipe[0]);

  if (cnt == 0) return;
  int first = primes[0];
  printf("prime %d\n", first);

  for (int i = 1; i < cnt; i++) {
    if (primes[i] % first != 0) {
      char p = primes[i];
      write(rpipe[1], &p, 1);
    }
  }
  close(rpipe[1]);

  int pid = fork();
  if (pid == 0) {
    //child
    filter(rpipe);
  }
}

int
main()
{
  int lpipe[2];

  pipe(lpipe);

  for (int i = 2; i <= 35; i++) {
    char p = i;
    write(lpipe[1], &p, 1);
  }
  close(lpipe[1]);

  filter(lpipe);

  wait((int *) 0);

  exit(0);
}
