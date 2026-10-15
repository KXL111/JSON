#ifndef UDP_LINK_H
#define UDP_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    int         sock;
    uint32_t    peer_ip;
    uint16_t    peer_port;
} udp_link_t;

bool udp_link_open(udp_link_t *link, const char *name, uint16_t local_port,
                   const char *peer_ip, uint16_t peer_port, uint32_t rx_timeout_ms);

int  udp_link_recv(udp_link_t *link, void *buf, int size);

bool udp_link_send(udp_link_t *link, const void *data, int len);

#endif
