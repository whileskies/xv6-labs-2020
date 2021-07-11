## Lab11: networking

Lab11为xv6编写网卡驱动，使得网络协议栈（IP、UDP、ARP）能够正常运行。

#### networking

##### 实现

1. 实现`e1000_transmit()`

`e1000_transmit()`函数接收一mbuf，mbuf包含了将要发送的以太网帧。由于网卡采用DMA传输数据，要发送数据帧，要告诉DMA控制器数据帧的起始内存地址和长度。因此内存中维护了`tx_ring`环形传送描述队列，网卡寄存器维护了`E1000_TDH`作为队列头指针，`E1000_TDT`作为队列尾指针，结构如下图所示：

<img src="https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210711171635.png" alt="image-20210711171628026" style="zoom: 50%;" />

初始化时所有队列中的描述符状态均设为`E1000_TXD_STAT_DD`，根据提示，当tail的`E1000_TXD_STAT_DD`为0时，代表网卡还未取走该帧数据，则当前队列满了，返回错误。

网卡每次会从head处取数据，取完数据将`E1000_TXD_STAT_DD`状态设为1，代表传送完成，但是只有在描述符拥有`E1000_TXD_CMD_RS`命令时才设置`E1000_TXD_STAT_DD`。

此外以太网帧的MTU为1500，一个缓冲区的大小为2048，因此不需要进行帧的划分，因此每个传送数据的描述命令应加上`E1000_TXD_CMD_EOP`，代表数据包的结束。

可能多个线程同时调用`e1000_transmit()`来传送数据，需要为其加锁互斥访问。

代码如下：

```c
int
e1000_transmit(struct mbuf *m)
{
  acquire(&e1000_lock);

  int tail = regs[E1000_TDT];
  if(!(tx_ring[tail].status & E1000_TXD_STAT_DD)){
    release(&e1000_lock);
    return -1;
  }
  
  if(tx_mbufs[tail])
    mbuffree(tx_mbufs[tail]);
  
  memset(&tx_ring[tail], 0, sizeof(struct tx_desc));
  tx_ring[tail].cmd = (E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS);
  tx_ring[tail].addr = (uint64)m->head;
  tx_ring[tail].length = m->len;
  tx_mbufs[tail] = m;

  regs[E1000_TDT] = (tail + 1) % TX_RING_SIZE;
  
  release(&e1000_lock);
 
  return 0;
}
```

2. 实现实现`e1000_recv()`

网卡接收到数据并根据`rx_ring`环形接收描述队列将数据通过DMA传送到内存指定位置，设置`E1000_RXD_STAT_DD`标记，并产生中断，`e1000_recv()`被调用。

该函数需要扫描`rx_ring`传递每一个接收到的包到网络协议栈，调用`net_rx()`函数传送，之后应该分配一个新的缓冲区替换到相应描述符中。

`e1000_transmit()`和`e1000_recv()`并不共享数据结构，相互独立，不会相互影响，因此可以为`e1000_recv()`单独加上另一把锁，防止在一个线程在`e1000_recv()`执行时，另外一个CPU产生了`e1000_intr`中断。

代码如下：

```c
struct spinlock e1000_lockrx;

// e1000_init()
initlock(&e1000_lockrx, "e1000_rx");

static void
e1000_recv(void)
{
  acquire(&e1000_lockrx);

  int i = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  while(rx_ring[i].status & E1000_RXD_STAT_DD){
    rx_mbufs[i]->len = rx_ring[i].length;
    struct mbuf *rb = rx_mbufs[i];

    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
    rx_ring[i].status = 0;
    regs[E1000_RDT] = i;

    net_rx(rb);

    i = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  }

  release(&e1000_lockrx);
}
```

#### 实验测试

![image-20210711175600483](https://whileskies-pic.oss-cn-beijing.aliyuncs.com/20210711175600.png)

#### 代码

https://github.com/whileskies/xv6-labs-2020/tree/net

