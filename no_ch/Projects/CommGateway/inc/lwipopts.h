#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "FreeRTOSConfig.h"

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_PROVIDE_ERRNO              1
#define LWIP_STATS                      0

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_NETCONN                6
#define MEMP_NUM_NETBUF                 16
#define MEMP_NUM_TCPIP_MSG_INPKT        16
#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536

#define LWIP_ARP                        1
#define ARP_QUEUEING                    1
#define LWIP_ICMP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        0
#define LWIP_RAW                        0
#define LWIP_DHCP                       0
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              12
#define UDP_TTL                         255

#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             1
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1
#define LWIP_NETIF_STATUS_CALLBACK      0

#define TCPIP_THREAD_NAME               "TCP/IP"
#define TCPIP_THREAD_STACKSIZE          1024
#define TCPIP_THREAD_PRIO               (configMAX_PRIORITIES - 2)
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_THREAD_STACKSIZE        512
#define DEFAULT_UDP_RECVMBOX_SIZE       16
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS  1
#define LWIP_FREERTOS_CHECK_CORE_LOCKING              0

#if defined(__GNUC__) && !defined(__CC_ARM) && !defined(__ARMCC_VERSION)
#include <sys/time.h>
#define LWIP_TIMEVAL_PRIVATE            0
#endif

#endif
