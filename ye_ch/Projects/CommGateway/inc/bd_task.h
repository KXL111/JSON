/*
 * bd_task —— 北斗任务：本机 <-> 北斗 的全部业务（协议 3.md，设计 design.txt）
 *
 * 北斗设备经 RS422 串口接在 USART6 上（serial.c），本任务独占这个串口：
 *   读卡：周期发 ICR，收到 ICP 后记下服务频度、通信长度等级（只读一次）
 *   信号：接收北斗自动输出的 PWI（波束）和 GSS（卫星），每秒算一次 SQ / status；
 *         串口 5 秒没有有效语句判为异常
 *   发报：终端请求 -> UTF-8 转 GBK -> 混编 TCQ -> 等 FKI 回执 -> 结果交回终端
 *   收报：TCI -> 解码成 UTF-8 -> 交给终端任务组 JSON 上报（42214）
 */
#ifndef BD_TASK_H
#define BD_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void bd_init(void);                         /* 创建请求队列，在任务启动前调用 */
void bd_task(void *arg);

/* 终端任务调用：提交一条待发短报文（kind = TEXT_SEND）。队列满返回 false */
bool bd_submit(const text_msg_t *req);

#endif /* BD_TASK_H */
