/*
 * ethernetif —— lwIP 网卡驱动（STM32F407 ETH + LAN8720A，RMII 接口）
 *
 * 和以太网硬件有关的代码全部在 ethernetif.c 里：引脚、时钟、PHY 复位与自协商、MAC/DMA、
 * ETH 中断、接收任务，以及 lwIP 要求的 netif 收发接口。上层用法：
 *     while (!ethernetif_hw_init()) { 延时后重试 }        // 网线没插好会失败
 *     netif_add(&netif, &ip, &mask, &gw, NULL, ethernetif_init, tcpip_input);
 *
 * PHY 地址、复位脚在 ethernetif.c 开头（PHY_ADDR、PHY_RST_xxx）；
 * PHY 状态寄存器的位定义在 stm32f4x7_eth_conf.h（PHY_SR / PHY_SPEED_STATUS / PHY_DUPLEX_STATUS），换 PHY 时改这两处。
 */
#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include <stdbool.h>
#include "lwip/err.h"
#include "lwip/netif.h"

bool  ethernetif_hw_init(void);
err_t ethernetif_init(struct netif *netif);

#endif /* ETHERNETIF_H */
