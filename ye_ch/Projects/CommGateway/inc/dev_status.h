/*
 * dev_status —— 设备状态总表（北斗 + 天通），心跳回复从这里取数
 *
 * 设备任务负责写，终端任务负责读。所有访问都要加锁：
 *     dev_status_t *s = dev_status_lock();
 *     s->bd.sq = 3;
 *     dev_status_unlock();
 * 只读时用 dev_status_read() 拷一份快照，锁持有时间最短。
 */
#ifndef DEV_STATUS_H
#define DEV_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include "app_msg.h"

/* 北斗 IC 卡信息（ICP），上电读到一次后不再查询 */
typedef struct {
    bool     valid;             /* 已读到卡 */
    char     id[16];            /* 本机北斗卡号 */
    uint16_t freq_region;       /* 区域短报文服务频度(秒)，无区域权限为 0 */
    uint16_t freq_global;       /* 全球短报文服务频度(秒)，无全球权限为 0 */
    uint8_t  len_level;         /* 通信长度等级 1~5 */
    uint16_t len_cn;            /* 该等级可发汉字数 */
    uint16_t len_en;            /* 该等级可发字符数 */
} bd_card_t;

typedef struct {
    /* 心跳里上报的字段 */
    uint8_t   status;           /* 0 在线，1 离线（SQ 为 0），2 异常（串口 5 秒没有有效语句） */
    char      msg[32];          /* 消息内容，默认空 */
    uint16_t  cd;               /* 冷却时间(秒)，暂取全球短报文服务频度 */
    uint8_t   sq;               /* 信号等级 0~4 */
    uint16_t  maxlen;           /* 可发字符数，暂取汉字上限 */
    /* 读卡记录 */
    bd_card_t card;
} bd_status_t;

/* 天通：先占位，等天通任务实现后再填 */
typedef struct {
    uint8_t   status;           /* 0 入网成功，1 未入网，2 异常 */
    char      msg[32];
    uint8_t   sq;               /* 信号等级 0~4 */
    uint16_t  maxlen;           /* 可发字符数 */
} tt_status_t;

/* 正在发送的短报文任务（心跳里的 taskstate） */
typedef struct {
    char      taskid[APP_ID_LEN];   /* 空串表示没有任务 */
    uint8_t   progress;             /* 发送进度 0~100 */
} task_state_t;

typedef struct {
    bd_status_t  bd;
    tt_status_t  tt;
    task_state_t task;
} dev_status_t;

void          dev_status_init(void);
dev_status_t *dev_status_lock(void);
void          dev_status_unlock(void);
void          dev_status_read(dev_status_t *out);

#endif /* DEV_STATUS_H */
