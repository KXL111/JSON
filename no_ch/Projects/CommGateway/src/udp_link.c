#include <string.h>
#include "lwip/sockets.h"
#include "log.h"
#include "udp_link.h"

bool udp_link_open(udp_link_t *link, const char *name, uint16_t local_port,
                   const char *peer_ip, uint16_t peer_port, uint32_t rx_timeout_ms)
{
    struct sockaddr_in local;
    struct in_addr     peer;
    int                timeout = (int)rx_timeout_ms;

    if (!inet_aton(peer_ip, &peer)) {
        LOG("%s: bad peer ip \"%s\" in app_config.h", name, peer_ip);
        return false;
    }
    link->name      = name;
    link->peer_ip   = peer.s_addr;
    link->peer_port = peer_port;
    link->sock      = socket(AF_INET, SOCK_DGRAM, 0);
    if (link->sock < 0) {
        LOG("%s: socket failed", name);
        return false;
    }

    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(link->sock, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        setsockopt(link->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        LOG("%s: bind port %u failed", name, local_port);
        closesocket(link->sock);
        link->sock = -1;
        return false;
    }

    LOG("%s: udp %u <-> %s:%u", name, local_port, peer_ip, peer_port);
    return true;
}

int udp_link_recv(udp_link_t *link, void *buf, int size)
{
    struct sockaddr_in from;
    socklen_t          fromlen = sizeof(from);
    char               ip[16];
    int                n;

    n = recvfrom(link->sock, buf, (size_t)size, 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        return 0;
    }
    if (from.sin_addr.s_addr != link->peer_ip) {
        LOG("%s: drop %d bytes from %s:%u (not the configured peer ip)", link->name, n,
            inet_ntoa_r(from.sin_addr, ip, sizeof(ip)), (unsigned)ntohs(from.sin_port));
        return 0;
    }
    return n;
}

bool udp_link_send(udp_link_t *link, const void *data, int len)
{
    struct sockaddr_in to;

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons(link->peer_port);
    to.sin_addr.s_addr = link->peer_ip;
    if (sendto(link->sock, data, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to)) != len) {
        LOG("%s: send %d bytes failed", link->name, len);
        return false;
    }
    return true;
}
