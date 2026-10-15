/*
 * net.c —— 网络初始化：网卡硬件 -> lwIP 协议栈 -> 添加网卡（静态 IP）
 */
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "app_config.h"
#include "ethernetif.h"
#include "log.h"
#include "net.h"

#define NET_RETRY_MS    2000

int errno;                      /* lwIP 使用自带的 errno 定义（lwipopts.h: LWIP_PROVIDE_ERRNO） */

static struct netif s_netif;    /* 唯一的网卡，lwIP 一直引用它，所以是静态变量 */

/*
 * 网络初始化：网卡硬件 -> lwIP 协议栈 -> 添加网卡（静态 IP），阻塞到全部完成
 *
 * 步骤：
 *   1. ethernetif_hw_init() 失败（网线没插、PHY 没就绪）就打日志，2s 后重试，直到成功；
 *   2. tcpip_init()：初始化 lwIP，创建 TCP/IP 任务（优先级 6）；
 *   3. 把 app_config.h 里的 IP / 掩码 / 网关字符串转成地址，格式不对时打日志
 *      （出错的那一项和它后面还没解析的项都保持 0.0.0.0）；
 *   4. 持有 TCP/IP 内核锁调用 netif_add()（内部回调 ethernetif_init()：描述符、ETH_RX 任务、中断），
 *      设为默认网卡，netif_set_up() 启用。返回后各任务就可以用 socket 了。
 * 等网线期间 TERM 还没创建、没登记看门狗；BD / TT 在自己的任务里照常喂狗，所以等网线不会导致复位。
 *
 * 调用：main.c 的 start_task()，运行在 START 任务里
 */
void net_init(void)
{
    ip4_addr_t ip = {0}, mask = {0}, gw = {0};

    while (!ethernetif_hw_init()) {
        LOG("NET: ethernet not ready (cable/PHY), retry in %d ms", NET_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
    }

    tcpip_init(NULL, NULL);
    if (!ip4addr_aton(CFG_LOCAL_IP, &ip) || !ip4addr_aton(CFG_LOCAL_NETMASK, &mask) ||
        !ip4addr_aton(CFG_LOCAL_GATEWAY, &gw)) {
        LOG("NET: bad local ip/netmask/gateway in app_config.h");
    }

    LOCK_TCPIP_CORE();                      /* netif_* 不是线程安全的，TCP/IP 任务已在运行，要加锁 */
    netif_add(&s_netif, &ip, &mask, &gw, NULL, ethernetif_init, tcpip_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    UNLOCK_TCPIP_CORE();

    LOG("NET: up, ip %s", CFG_LOCAL_IP);
}
