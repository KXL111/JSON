/*
 * app_msg.h —— 任务之间传递的短报文消息
 *
 * 终端任务和设备任务（北斗、天通）之间只通过队列传这一种结构，
 * 各自的 socket、协议细节都留在自己的模块里：
 *   终端 -> 设备：TEXT_SEND    发一条短报文
 *   设备 -> 终端：TEXT_RESULT  这条短报文的发送结果（回执）
 *                 TEXT_RECV    设备收到的短报文
 */
#ifndef APP_MSG_H
#define APP_MSG_H

#define APP_ID_LEN      64      /* msg_id / taskid 最大长度（含结束符） */
#define APP_PEER_LEN    24      /* 对方号码最大长度（含结束符） */
#define APP_TEXT_LEN    3072    /* 正文最大字节数（UTF-8，1000 个汉字约 3000 字节） */

typedef enum {
    DEV_BD = 0,                 /* 北斗 */
    DEV_TT,                     /* 天通 */
    DEV_COUNT
} dev_id_t;

typedef enum {
    TEXT_SEND = 0,
    TEXT_RESULT,
    TEXT_RECV
} text_kind_t;

typedef struct {
    text_kind_t kind;
    dev_id_t    dev;
    char        msg_id[APP_ID_LEN];     /* 终端请求的 msg_id，回执时原样带回 */
    char        taskid[APP_ID_LEN];     /* 终端请求的 taskid，回执时原样带回 */
    char        peer[APP_PEER_LEN];     /* 发送：收信方号码；接收：发信方号码 */
    int         status;                 /* 仅 TEXT_RESULT：0 成功，1 失败 */
    char        text[APP_TEXT_LEN];     /* 正文（UTF-8）；TEXT_RESULT 时是失败原因 */
} text_msg_t;

#endif /* APP_MSG_H */
