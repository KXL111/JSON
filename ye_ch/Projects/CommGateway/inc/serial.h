/*
 * serial —— 设备串口驱动：北斗走 RS422，天通走 RS232
 *
 * RS422 / RS232 只是板上电平转换芯片不同，MCU 这一侧都是普通 USART，两者都是全双工，
 * 不需要收发方向控制脚，所以共用一套驱动，只是用的 USART、引脚、DMA 数据流不同（见 serial.c 的 k_hw[]）。
 *
 * 接收 —— DMA 循环模式 + 环形缓冲区 + 空闲中断：
 *   DMA 把每个收到的字节依次写进环形缓冲区，写到末尾自动绕回开头，DMA 计数器就是"写指针"；
 *   任务读到哪里记在 rx_tail（"读指针"），两个指针之间就是还没读的数据。
 *   串口线路空闲（一串数据收完）时进一次中断，释放信号量唤醒正在等数据的任务。
 *   这样 CPU 不用每个字节进一次中断，任务晚一点来读也不会丢数据（北斗缓冲约存 1 秒的数据）。
 * 发送 —— DMA 普通模式：
 *   数据先拷进驱动自己的发送缓冲，启动 DMA 后立即返回，调用方的缓冲马上可以复用；
 *   上一包还没发完时，先等它发完（最多等 timeout_ms）。
 *
 * 用法（每个串口只在一个任务里打开、读、写，不需要加锁）：
 *     serial_open(SERIAL_BD, CFG_BD_BAUDRATE);
 *     n = serial_read(SERIAL_BD, buf, sizeof(buf), 20);    // 最多等 20ms
 *     serial_write(SERIAL_BD, line, len, 1000);
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SERIAL_BD = 0,          /* 北斗：USART6，PC6 发 / PC7 收，外接 TTL 转 RS422 模块 */
    SERIAL_TT,              /* 天通：USART3，PB10 发 / PB11 收，接板载 SP3232（COM3，RS232） */
    SERIAL_COUNT
} serial_id_t;

/* 初始化引脚、USART（8N1）、收发 DMA 和空闲中断。参数不对返回 false */
bool   serial_open(serial_id_t id, uint32_t baudrate);

/* 取出已收到的数据，最多 size 字节；没有数据时最多等 timeout_ms。返回取到的字节数，超时返回 0 */
size_t serial_read(serial_id_t id, void *buf, size_t size, uint32_t timeout_ms);

/* 用 DMA 发送 len 字节，启动后立即返回。上一包 timeout_ms 内没发完、或 len 超过发送缓冲返回 false */
bool   serial_write(serial_id_t id, const void *data, size_t len, uint32_t timeout_ms);

#endif /* SERIAL_H */
