#include "kernel/types.h"
#include "user.h"

int
main(int argc, char *argv[]) {
  int time;

  if (argc != 2) {
    fprintf(2, "Usage: sleep time\n\n");
    exit(1);
  }

  time = atoi(argv[1]);
  sleep(time);
  exit(0);
}

