/*
 * net —— 网络初始化：lwIP 协议栈 + 网卡，本机静态 IP 取自 app_config.h
 */
#ifndef NET_H
#define NET_H

/* 阻塞直到网卡就绪；网线没插好时每 2 秒重试一次 */
void net_init(void);

#endif /* NET_H */
