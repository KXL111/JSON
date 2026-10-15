/*
 * udp_link —— 点对点 UDP 链路：本机端口 <-> 一个固定对端（IP:端口）
 *
 * 每条链路只在一个任务里使用（打开、收、发都在该任务里），不需要加锁。
 * 只接收来自对端 IP 的数据，其他来源的包直接丢弃并打印日志。
 */
#ifndef UDP_LINK_H
#define UDP_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;           /* 日志里显示的链路名 */
    int         sock;
    uint32_t    peer_ip;        /* 对端 IP（网络字节序） */
    uint16_t    peer_port;      /* 对端端口 */
} udp_link_t;

/* 绑定本机端口；rx_timeout_ms 为 udp_link_recv 最长等待时间 */
bool udp_link_open(udp_link_t *link, const char *name, uint16_t local_port,
                   const char *peer_ip, uint16_t peer_port, uint32_t rx_timeout_ms);

/* 返回收到的字节数；超时或丢弃返回 0 */
int  udp_link_recv(udp_link_t *link, void *buf, int size);

bool udp_link_send(udp_link_t *link, const void *data, int len);

#endif /* UDP_LINK_H */
