/*
 * term_task —— 终端任务：本机 <-> 终端（通信综合管理软件）的 JSON 协议，见 1.docx；链路是 UDP
 *
 * 终端发来：42201 心跳请求    -> 回 42202，附带北斗/天通状态
 *           42212 北斗文本传输 -> 交给北斗任务，结果回来后回 42213
 *           42208 天通文本传输 -> 交给天通任务（暂未接入，直接回 42209 失败）
 *           42211/42215 等     -> 终端对本机上报的确认，只打日志
 *           其他请求           -> 本机暂不支持，回 "请求号+1"、status=1
 * 设备发来：发送结果 -> 42213/42209（带回终端请求的 msg_id / taskid）
 *           收到的短报文 -> 组 JSON 上报 42214/42210，msg_id、taskid 各生成一个标准 UUID
 */
#ifndef TERM_TASK_H
#define TERM_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void term_init(void);                       /* 创建消息队列，在任务启动前调用 */
void term_task(void *arg);

/* 设备任务调用：把发送结果（TEXT_RESULT）或收到的短报文（TEXT_RECV）交给终端任务 */
bool term_post(const text_msg_t *msg);

#endif /* TERM_TASK_H */
