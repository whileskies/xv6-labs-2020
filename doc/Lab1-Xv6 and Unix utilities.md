## Lab1: Xv6 and Unix utilities

Lab1主要是了解并运行RiscV版本的XV6，并为其添加一些用户态程序

#### Boot xv6 (easy)

使用`make qemu`运行xv6，并运行`ls`等程序

#### sleep (easy)

##### 任务

调用内核sleep系统调用完成功能

```c
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
```

在Makefile的`UPROGS`中添加`$U/_sleep\` 

#### pingpong (easy)

##### 任务

创建一对父子进程，该对进程使用管道通信

```c
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
```

#### primes (moderate)/(hard)

##### 任务

使用`pipe`、`fork`组成一管道过滤器，第一个进程输出2，并过滤2-35中2的倍数的数，第二个进程输出3并过滤3的倍数的数...

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210321224328.png" alt="image-20210321224322050" style="zoom: 80%;" />

##### 过程

首先main函数初始管道，将2-35写入管道，并关闭写管道，调用filter函数处理，filter函数用于递归处理该问题，参数为上一层管道数组，从管道读端读入上一层的数字，并创建一个新管道，对数字过滤后传入下一层，下一层即为通过fork创建的子进程

```c
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
```

##### 运行结果

![image-20210321225351704](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210321225353.png)

#### find (moderate)

##### 任务

在一个目录树中搜索特定名称的所有文件

##### 过程

递归搜索目录中的指定文件即可，增加了`.`、`*`的通配符功能，正则匹配代码参考自[LeetCode 44题](https://leetcode-cn.com/problems/wildcard-matching/)

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// int
// match(char *filename, char *name) {
//   return strcmp(filename, name) == 0;
// }

// '.' Matches any single character.​​​​
// '*' Matches zero or more of the preceding element.
int match(char* s, char* p) {
  if (!*p) return !*s;
  if (*(p + 1) != '*') 
    return *s == *p || (*p == '.' && *s != '\0') ? match(s + 1, p + 1) : 0; 
  else 
    return *s == *p || (*p == '.' && *s != '\0') ? match(s, p + 2) || match(s + 1, p) : match(s, p + 2);
    //return (*s == *p || (*p == '.' && *s != '\0')) && match(s + 1, p) || match(s, p + 2);
}


void
catdir(char *predix, char *name, char *buf)
{
  memcpy(buf, predix, strlen(predix));
  char *p = buf + strlen(predix);
  *p++ = '/';
  memcpy(p, name, strlen(name));
  p += strlen(name);
  *p++ = 0;
}


void
find(int fd, char *dir, char *name) {
  struct dirent de;
  
  while(read(fd, &de, sizeof(de)) == sizeof(de)) {
    if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
      continue;
    struct stat st;
    char path[512];
    catdir(dir, de.name, path);
  
    if(de.inum == 0)
        continue;
    if(stat(path, &st) < 0){
        printf("find: cannot stat %s\n", path);
        continue;
    }
    if (st.type == T_FILE && match(de.name, name)) {
      printf("%s\n", path);
    } else if (st.type == T_DIR) {
      int subfd;
      if((subfd = open(path, 0)) < 0){
        printf("find: cannot open %s\n", path);
        continue;
      }
      find(subfd, path, name);
    }

  }
}


int
main(int argc, char *argv[])
{
  if (argc != 3) {
    fprintf(2, "Usage: find dir name\n");
    exit(1);
  }

  char dir[DIRSIZ + 1];
  char name[DIRSIZ + 1];

  if (strlen(argv[1]) > DIRSIZ || strlen(argv[2]) > DIRSIZ) {
    fprintf(2, "dir or name too long...\n");
    exit(1);
  }

  memcpy(dir, argv[1], strlen(argv[1]));
  memcpy(name, argv[2], strlen(argv[2]));

  int fd;
  struct stat st;

  if((fd = open(dir, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", dir);
    exit(1);
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", dir);
    close(fd);
    exit(1);
  }

  if (st.type != T_DIR) {
    printf("%s is not a dir\n", dir);
  } else {
    find(fd, dir, name);
  }
  
  exit(0);
}
```

##### 运行结果

![image-20210322091152189](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210322091153.png)

#### xargs (moderate)

##### 任务

编写一简单版本的UNIX xargs程序，该程序从标准输入中读取多行并将每一行作为一指定程序的参数

##### 过程

xargs从标准输入中读多行，多行按照`\n`分隔，使用`exec`系统调用创建子进程执行指定程序，并将标准输入读的参数和该程序原本的参数一并作为该程序参数

```c
#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

#define MAX_ARG_LEN 512

void
copy_argv(char **ori_argv, int ori_argc, char *new_argv, char **argv)
{
  int k = 0;
  for (int i = 0; i < ori_argc; i++) {
    argv[k] = malloc(strlen(ori_argv[i]) + 1);
    memcpy(argv[k++], ori_argv[i], strlen(ori_argv[i]) + 1);
  }
  argv[k] = malloc(strlen(new_argv) + 1);
  memcpy(argv[k++], new_argv, strlen(new_argv) + 1);
}


void
print(char **s, int n)
{
  for (int i = 0; i < n; i++) {
    printf("%s\n", s[i]);
  }
}

int 
main(int argc, char *argv[])
{
  if (argc <= 1) {
    fprintf(2, "Usage: xargx command [arg ...]\n");
    exit(1);
  }

  char param[MAX_ARG_LEN];
  int i = 0;
  char ch;
  int ignore = 0;
  while (read(0, &ch, 1) > 0) {
    if (ch == '\n') {
      if (ignore) {
        i = 0;
        ignore = 0;
        continue;
      }
      param[i] = 0;
      i = 0;

      int pid = fork();
      if (pid == 0) {
        //child
        int cmd_argc = argc;
        
        char *cmd_argv[MAXARG];

        copy_argv(argv + 1, argc - 1, param, cmd_argv);
        cmd_argv[cmd_argc] = 0;
        
        exec(cmd_argv[0], cmd_argv);

        exit(0);
      } else {
        wait((int *)0);
      }
      
    } else {
      
      if (!ignore && i >= MAX_ARG_LEN - 1) {
        printf("xargs: too long arguments...\n");
        ignore = 1;
      }

      if (!ignore) {
        param[i++] = ch;
      }
    }
  }

  exit(0);
}
```

##### 运行结果

![image-20210322092734417](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210322092735.png)

#### 实验测试

![image-20210322093022342](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210322093022.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/util