#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include <stdbool.h>
#include "lwip/err.h"
#include "lwip/netif.h"

bool  ethernetif_hw_init(void);
err_t ethernetif_init(struct netif *netif);

#endif
