#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

int check(int n) {
  if (n % 2 == 0)
    return 1;
  else
    return 0;
}

int cnt(int n) {
  int sum = 0;
  for (int i = 0; i <= n; i++) {
    if (check(i))
      sum += i;
  }
  
  return sum;
}

void demo() {
  int sum = cnt(10);
  printf("demo: %d\n", sum);
}