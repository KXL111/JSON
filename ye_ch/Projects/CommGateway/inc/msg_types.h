/*
 * msg_types.h —— 终端（通信综合管理软件）<-> 本机 的 JSON 消息类型，见 1.docx
 *
 * 规律：终端的请求是 A，本机的回复是 A+1；本机主动上报是 B，终端的确认是 B+1。
 */
#ifndef MSG_TYPES_H
#define MSG_TYPES_H

#define MSG_HEARTBEAT_REQUEST       42201   /* EXT01 心跳请求 */
#define MSG_HEARTBEAT_REPLY         42202   /* EXT02 心跳返回 */
#define MSG_ALARM_REPORT            42203   /* EXT03 告警 */
#define MSG_VHF_CHANNEL_SET         42204   /* EXT04 VHF 配置 */
#define MSG_VHF_CHANNEL_REPLY       42205
#define MSG_MESH_POWER_SET          42206   /* EXT06 MESH 配置 */
#define MSG_MESH_POWER_REPLY        42207
#define MSG_TT_MESSAGE_SEND         42208   /* EXT08 天通文本传输 */
#define MSG_TT_MESSAGE_REPLY        42209
#define MSG_TT_MESSAGE_RECV         42210   /* EXT10 天通文本接收 */
#define MSG_TT_MESSAGE_RECV_ACK     42211
#define MSG_BD_MESSAGE_SEND         42212   /* EXT12 北斗文本传输 */
#define MSG_BD_MESSAGE_REPLY        42213
#define MSG_BD_MESSAGE_RECV         42214   /* EXT14 北斗文本接收 */
#define MSG_BD_MESSAGE_RECV_ACK     42215
#define MSG_TT_CALL_OUT             42216   /* EXT16 主动拨号 */
#define MSG_TT_CALL_OUT_REPLY       42217
#define MSG_TT_CALL_IN              42218   /* EXT18 来电 */
#define MSG_TT_CALL_IN_ACK          42219
#define MSG_TT_CALL_ACCEPT          42220   /* EXT20 接通 */
#define MSG_TT_CALL_ACCEPT_REPLY    42221
#define MSG_TT_CALL_REJECT          42222   /* EXT22 挂断 */
#define MSG_TT_CALL_REJECT_REPLY    42223
#define MSG_TT_CALL_ENDED           42224   /* EXT24 被挂断 */
#define MSG_TT_CALL_ENDED_ACK       42225
#define MSG_TT_POWER_SET            42226   /* EXT26 天通开关配置 */
#define MSG_TT_POWER_REPLY          42227

/*
 * 回复（42213 / 42209 / 不支持的请求等）content 里失败原因的键名。
 * 1.docx 写的是 "errormsg"；09 测试记录 test.txt 里这些回复用的是 "error_msg"（心跳 42202 两边都是 "errormsg"）。
 * 按接口文档用 "errormsg"；联调时终端如果显示不出失败原因，把这里改成 "error_msg" 即可，别处不用动。
 */
#define KEY_REPLY_ERRMSG            "errormsg"

#endif /* MSG_TYPES_H */
