## Lab9: file system

lab9是为xv6文件系统增加支持大文件的功能以及软链接功能。

#### Large files

##### 任务

当前的xv6文件被限制在268个块内， 每一块1024字节，这个限制由于xv6的inode结点包含12个直接块号，一个单一的间接块号，间接块号可以存储256个数据块块号，因此总共是12 + 256 = 268块。

该部分需要增加xv6文件的最大大小，通过为xv6引入二级间接块号来增加文件大小最大值，二级间接块号包括256个一级间接块号，每个一级间接块号又可以存储256个数据块块号，因此最大文件可以达到65803 块（256 * 256 + 256 + 11块，牺牲了一个直接块号作为二级间接块号地址）。

##### 过程

1. 修改inode结构

由于需要引入二级间接块号，需要牺牲一个直接块号，因此需要修改inode的结构以修改布局，修改后inode有11个直接块号、1个一级间接块号地址、1个二级间接块号地址，代码如下：

```c
// fs.h
#define NDIRECT 11
#define DINDIRECTI (NDIRECT + 1)
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT

// file.h
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+2];
};
```

2. 修改`fs.c/bmap()`

`bmap()`函数将文件内块号映射为磁盘块号，如果不存在则会为其从bitmap寻找一空闲数据块，并将寻找到的数据块号添加到inode中，建立相应的映射。

如果文件内块号位于一级间接块号的范围内，则应该先根据一级间接块号地址找到存放一级间接块号的数据块，如果不存在则分配，再根据一级间接块号中的块号地址，找到具体的数据块，如果不存在则分配。

二级间接块号也是类似的，只不过多了一层查找过程，修改后的inode结构如下图所示：

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210630153348.png" alt="image-20210630153348525" style="zoom: 67%;" />

代码如下：

```c
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, addr2, *a, *a2;
  struct buf *bp, *bp2;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  
  bn -= NINDIRECT;

  if(bn < NINDIRECT * NINDIRECT){
    uint dbn = bn / NINDIRECT;
    uint dbnoff = bn % NINDIRECT;

    // Load doubly-indirect block, allocating if necessary.
    if((addr = ip->addrs[DINDIRECTI]) == 0)
      ip->addrs[DINDIRECTI] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    
    a = (uint*)bp->data;
    if((addr2 = a[dbn]) == 0){
      a[dbn] = addr2 = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);

    //printf("addr2: %d\n", addr2);
    bp2 = bread(ip->dev, addr2);
    a2 = (uint*)bp2->data;
    if((addr = a2[dbnoff]) == 0){
      a2[dbnoff] = addr = balloc(ip->dev);
      log_write(bp2);
    }
    brelse(bp2);

    return addr;
  }

  panic("bmap: out of range");
}
```

3. 修改`fs.c/itrunc()`

有增加映射就有删除映射，`itrunc()`用于删除整个文件内容，则需要查找inode，删除所占有的所有数据块，包括一级间接块、二级间接块，代码如下：

```c
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp, *bp2;
  uint *a, *a2;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if(ip->addrs[DINDIRECTI]){
    bp = bread(ip->dev, ip->addrs[DINDIRECTI]);
    a = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      if(a[i]){
        bp2 = bread(ip->dev, a[i]);
        a2 = (uint*)bp2->data;
        for(j = 0; j < NINDIRECT; j++){
          if(a2[j])
            bfree(ip->dev, a2[j]);
        }
        brelse(bp2);
        bfree(ip->dev, a[i]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[DINDIRECTI]);
    ip->addrs[DINDIRECTI] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
```

4. 修改`file.c/filewrite()`

xv6为了维持文件系统的一致性，对于文件系统的调用都将被作为一个事务来完成，由于日志大小限制，一个事务同时更新的数据块数量也受到了限制，对于大文件的写来说，将被切分成多次写，每次写几个数据块作为一个事务进行提交。

对于文件的一次写，可能需要为其分配数据块，那么存储间接块号的数据块、inode数据块都需要更新，每个事务实际使用的数据块数量应该考虑到同时更新的这些数据块；而由于添加了二级间接块号的数据块，那么同时又可能需要更新二级间接块号的数据块，因此最终每个事务使用的最大数据块又应该从每个事务的日志最大块数 -1。

```c
int max = ((MAXOPBLOCKS-1-2-2) / 2) * BSIZE;
```

#### Symbolic links

##### 任务

该部分为xv6增加符号链接（软链接），符号链接通过文件路径链接另一个文件；当一个符号链接打开时，内核打开实际链接到的文件。符号链接像硬链接，但是硬链接被限制指向同一个磁盘上，符号链接可以在不同的磁盘设备上。

##### 过程

1. 增加系统调用

按照之前Lab添加系统调用的方式添加`symlink`系统调用。

2. 增加符号链接文件类型

```c
// stat.h
#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device
#define T_SYMLINK 4   // Symbolic link

// fcntl.h 
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
#define O_NOFOLLOW 0x800 
```

3. 创建`sysfile.c/sys_symlink()`系统调用函数

符号链接是一个特殊的文件，文件类型为T_SYMLINK，该文件只需要存储另一个文件的路径即可，因此首先先创建一个符号链接文件，再将所链接文件路径写到符号链接文件中即可。代码如下：

```c
uint64
sys_symlink(void)
{
  char new[MAXPATH], old[MAXPATH];
  struct inode *op, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((op = namei(old)) != 0){
    ilock(op);
    if(op->type == T_DIR){
      iunlockput(op);
      end_op();
      return -1;
    }
    iunlockput(op);
  }

  if((ip = create(new, T_SYMLINK, 0, 0)) == 0){
    end_op();
    return -1;
  }

  uint len = strlen(old) + 1;
  if(writei(ip, 0, (uint64)old, 0, len) != len){
    iunlockput(ip);
    end_op();
    return -1;
  }

  iupdate(ip);
  iunlockput(ip);
  end_op();

  return 0;
}
```

4. 修改`sysfile.c/sys_open()`增加链接文件打开方式

进程使用`open()`系统调用打开符号链接文件时，可以有两种打开方式：

- 打开模式为`O_NOFOLLOW`时，直接打开符号链接文件的内容，也即另一个文件的文件路径，直接按照一般文件打开即可
- 打开模式不加`O_NOFOLLOW`时，则打开所链接的文件，如果所链接的文件又是一个链接文件，这应该递归地去找到真正的文件

`sys_open()`函数修改如下：

```c
if(omode & O_NOFOLLOW){
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
} else{
  char rpath[MAXPATH];
  if((ip = find_symlink(path, rpath, 0)) == 0){
    end_op();
    return -1;
  }
}
```

`find_symlink()`函数递归找到真正的文件，如果递归层数过多，则停止：

```c
struct inode*
find_symlink(char *path, char *rpath, int depth)
{
  if(depth >= 10)
    return 0;

  struct inode *ip;
  if((ip = namei(path)) != 0){
    ilock(ip);

    if(ip->type != T_SYMLINK){
      iunlock(ip);
      return ip;
    }

    if(readi(ip, 0, (uint64)rpath, 0, ip->size) == 0){
      iunlockput(ip);
      return 0;
    }

    iunlockput(ip);
  
    return find_symlink(rpath, rpath, depth + 1);
  }

  return 0;
}
```

#### 实验测试

![test2](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210630161137.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/fs

