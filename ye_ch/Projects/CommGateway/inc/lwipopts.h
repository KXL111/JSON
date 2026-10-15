/*
 * lwipopts.h —— lwIP 配置：FreeRTOS + socket 接口，只用 UDP（加 ICMP 方便 ping）
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "FreeRTOSConfig.h"

/* ---------------- 系统 ---------------- */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_PROVIDE_ERRNO              1       /* errno 变量定义在 net.c */
#define LWIP_STATS                      0

/* ---------------- 内存 ---------------- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)     /* 发送的数据从这里分配，单包最大约 4KB */
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_NETCONN                6
#define MEMP_NUM_NETBUF                 16
#define MEMP_NUM_TCPIP_MSG_INPKT        16
#define PBUF_POOL_SIZE                  16              /* 接收用，每个放得下一整帧 */
#define PBUF_POOL_BUFSIZE               1536

/* ---------------- 协议 ---------------- */
#define LWIP_ARP                        1
#define ARP_QUEUEING                    1               /* ARP 解析期间缓存多个包，大报文分片才不会丢 */
#define LWIP_ICMP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        0
#define LWIP_RAW                        0
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define IP_REASSEMBLY                   1               /* 超过 1472 字节的 UDP 会被分片，收发都要支持 */
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              12
#define UDP_TTL                         255

/* ---------------- 接口 ---------------- */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             1               /* 可直接用 socket()/bind()/sendto() 等名字 */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0               /* 不定义 read/write/close 宏，避免和 C 库冲突 */
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1               /* SO_RCVTIMEO 直接用毫秒整数 */
#define LWIP_NETIF_STATUS_CALLBACK      0

/* ---------------- 线程 ---------------- */
#define TCPIP_THREAD_NAME               "TCP/IP"
#define TCPIP_THREAD_STACKSIZE          1024            /* 字 */
#define TCPIP_THREAD_PRIO               (configMAX_PRIORITIES - 2)
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_THREAD_STACKSIZE        512
#define DEFAULT_UDP_RECVMBOX_SIZE       16
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS  1
#define LWIP_FREERTOS_CHECK_CORE_LOCKING              0

/* GCC(newlib) 自带 struct timeval，Keil(ARMCC) 用 lwIP 自己的定义 */
#if defined(__GNUC__) && !defined(__CC_ARM) && !defined(__ARMCC_VERSION)
#include <sys/time.h>
#define LWIP_TIMEVAL_PRIVATE            0
#endif

#endif /* LWIPOPTS_H */
