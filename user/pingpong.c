#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main() {
  int p2c[2], c2p[2];
  int child_id;
  char *ping = "ping";
  char *pong = "pong";
  char buf[512] = {0};

  pipe(p2c);
  pipe(c2p);

  child_id = fork();
  if (child_id != 0) {
    //parent
    close(p2c[0]);
    close(c2p[1]);

    write(p2c[1], ping, strlen(ping));

    //wait((int *)0);
    read(c2p[0], buf, sizeof(buf));
    printf("%d: received %s\n", getpid(), buf);
    exit(0);
  } else {
    //child
    close(p2c[1]);
    close(c2p[0]);

    read(p2c[0], buf, sizeof(buf));
    printf("%d: received %s\n", getpid(), buf);

    write(c2p[1], pong, strlen(pong));
    exit(0);
  }

}