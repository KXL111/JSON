#ifndef STM32F4X7_ETH_CONF_H
#define STM32F4X7_ETH_CONF_H

#include "mcu.h"

#define _eth_delay_         ETH_Delay

#define PHY_RESET_DELAY     ((uint32_t)0x000FFFFF)
#define PHY_CONFIG_DELAY    ((uint32_t)0x00FFFFFF)

#define PHY_SR              ((uint16_t)31)
#define PHY_SPEED_STATUS    ((uint16_t)0x0004)
#define PHY_DUPLEX_STATUS   ((uint16_t)0x0010)

#endif
