/*
 * tt_task —— 天通任务（天通设备经 RS232 串口接在 USART3 上，协议还没到，业务先占位）
 *
 * 已经做好的：串口收发（serial.c）、按行切分收到的数据并打日志、看门狗、和终端任务之间的通道。
 * 天通协议到位后在 tt_task.c 里补：
 *   - 解析天通状态，写 dev_status 里的 tt（status / SQ / maxlength）
 *   - tt_submit() 改成入队，由天通任务发送；发送结果、收到的短信用 term_post()（dev = DEV_TT）
 *     交回终端任务，终端任务会自动组 JSON 回 42209 / 上报 42210（msg_id、taskid 自动生成 UUID）
 */
#ifndef TT_TASK_H
#define TT_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void tt_task(void *arg);

/* 终端任务调用：提交一条待发短信。天通还没接入，目前一律返回 false */
bool tt_submit(const text_msg_t *req);

#endif /* TT_TASK_H */
