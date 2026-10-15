#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "app_config.h"
#include "ethernetif.h"
#include "log.h"
#include "net.h"

#define NET_RETRY_MS    2000

int errno;

static struct netif s_netif;

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

    LOCK_TCPIP_CORE();
    netif_add(&s_netif, &ip, &mask, &gw, NULL, ethernetif_init, tcpip_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    UNLOCK_TCPIP_CORE();

    LOG("NET: up, ip %s", CFG_LOCAL_IP);
}
