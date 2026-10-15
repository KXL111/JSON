/*
 * stm32f4x7_eth_conf.h —— ST 以太网驱动（stm32f4x7_eth.c）的配置，按 LAN8720A 填写
 *
 * 由 stm32f4x7_eth.h 包含。驱动里用到的 PHY 相关常量在这里定义（参照实验 55 的同名文件）；
 * 收发缓冲个数和大小用驱动默认值：各 5 个、每个 ETH_MAX_PACKET_SIZE（1524）字节，
 * 缓冲本身定义在 ethernetif.c。
 */
#ifndef STM32F4X7_ETH_CONF_H
#define STM32F4X7_ETH_CONF_H

#include "mcu.h"

/* 不用增强型描述符（时间戳 / PTP 用不到），描述符每个 16 字节 */
/* #define USE_ENHANCED_DMA_DESCRIPTORS */

/* 驱动内部延时用它自带的空循环 ETH_Delay()。本工程不调用 ETH_Init()，PHY 的等待在 ethernetif.c 里用 vTaskDelay() 做 */
#define _eth_delay_         ETH_Delay

/* ETH_Init() 里 PHY 复位 / 配置后的延时（空循环次数），本工程不用，保留驱动需要的定义 */
#define PHY_RESET_DELAY     ((uint32_t)0x000FFFFF)
#define PHY_CONFIG_DELAY    ((uint32_t)0x00FFFFFF)

/*
 * PHY 状态寄存器：LAN8720A 是 31 号特殊状态寄存器，位 4:2 为速率 / 双工指示
 *   001 10M 半双工  101 10M 全双工  010 100M 半双工  110 100M 全双工
 * ethernetif.c 的 phy_read_mode() 用这三个定义判断协商结果。
 * 换 DP83848 时改为：PHY_SR 16，PHY_SPEED_STATUS 0x0002，PHY_DUPLEX_STATUS 0x0004。
 */
#define PHY_SR              ((uint16_t)31)
#define PHY_SPEED_STATUS    ((uint16_t)0x0004)      /* 1 = 10M */
#define PHY_DUPLEX_STATUS   ((uint16_t)0x0010)      /* 1 = 全双工 */

#endif /* STM32F4X7_ETH_CONF_H */
