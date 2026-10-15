/*
 * udp_link.c —— 点对点 UDP 链路（lwIP socket 接口）
 *
 * 北斗、天通改走串口后，只有终端链路还用 UDP：TERM 任务有一个 udp_link_t，
 * 打开、收、发都只在 TERM 任务里做，不需要加锁。模块本身是通用的，以后加 UDP 设备照样能用。
 */
#include <string.h>
#include "lwip/sockets.h"
#include "log.h"
#include "udp_link.h"

/*
 * 打开一条点对点 UDP 链路：校验对端 IP，创建 socket，绑定本机端口，设置接收超时
 *
 * 步骤：
 *   1. inet_aton() 把对端 IP 字符串转成网络字节序地址，格式不对直接失败（app_config.h 写错了）；
 *   2. socket(AF_INET, SOCK_DGRAM) 创建 UDP socket；
 *   3. bind() 到 0.0.0.0:local_port；
 *   4. SO_RCVTIMEO 设接收超时（lwipopts.h 配成直接传毫秒整数），使 udp_link_recv() 最多阻塞
 *      rx_timeout_ms，任务主循环才能按时喂狗、处理队列和定时事务。
 * bind 或 setsockopt 失败时关闭 socket 并返回 false，调用方每秒重试一次。
 *
 * 参数：link          —— 链路结构（调用方的静态变量）
 *       name          —— 日志里显示的链路名，如 "TERM"
 *       local_port    —— 本机端口
 *       peer_ip       —— 对端 IP 字符串，如 "10.9.0.24"
 *       peer_port     —— 对端端口
 *       rx_timeout_ms —— 接收超时
 * 返回：true 成功；false 失败（已打日志）
 * 调用：term_task() 启动时
 */
bool udp_link_open(udp_link_t *link, const char *name, uint16_t local_port,
                   const char *peer_ip, uint16_t peer_port, uint32_t rx_timeout_ms)
{
    struct sockaddr_in local;
    struct in_addr     peer;
    int                timeout = (int)rx_timeout_ms;   /* LWIP_SO_SNDRCVTIMEO_NONSTANDARD：毫秒整数 */

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

/*
 * 接收一个 UDP 包，最多阻塞 udp_link_open() 设定的超时时间
 *
 * 只接受源 IP 等于对端 IP 的包（不检查源端口），其他来源的包丢弃并打日志。
 * 包比 size 大时 lwIP 只拷贝前 size 字节，其余部分丢失。
 *
 * 参数：link        —— 已打开的链路
 *       buf / size  —— 接收缓冲及大小
 * 返回：收到的字节数；超时、出错或被丢弃返回 0
 * 调用：term_task() 主循环，每圈一次
 */
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

/*
 * 发送一个 UDP 包到对端 IP:端口
 *
 * 超过 1472 字节（以太网 MTU 1500 减去 IP、UDP 头）的包由 lwIP 自动分片，对端负责重组。
 * 第一次发给对端时 lwIP 要先做 ARP 解析，期间包会被缓存（ARP_QUEUEING）。
 *
 * 参数：link       —— 已打开的链路
 *       data / len —— 待发数据
 * 返回：true 全部发出；false 发送失败（已打日志）
 * 调用：term_task.c 的 msg_send()
 */
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
