/*
 * term_task.c —— 终端任务（终端链路仍是 UDP，见 udp_link.c）
 *
 * 主循环：收终端 JSON 并处理（最多等 20ms），再把设备任务交来的消息逐条组成 JSON 发给终端。
 * 终端的 socket 只在本任务里使用，设备任务通过 term_post() 的队列和这里交互。
 *
 * 函数调用关系（本文件的函数全部运行在 TERM 任务里，term_post() 除外）：
 *   term_task()                         任务主循环
 *     ├─ handle_json()                  处理终端发来的一条 JSON
 *     │    ├─ sync_time()               用报文里的 timestamp 校时
 *     │    ├─ uuid_generate()           请求里缺 msg_id / taskid 时各生成一个标准 UUID
 *     │    ├─ reply_heartbeat()         42201 -> 回 42202
 *     │    ├─ forward_text()            42212 / 42208 -> bd_submit() / tt_submit()，失败立即回复
 *     │    └─ reply_status()            本机不支持的请求 -> 回 "请求号 + 1"
 *     └─ handle_device_msg()            处理设备任务交来的 text_msg_t
 *          ├─ reply_status()            发送结果 -> 42213 / 42209（带回终端请求的 msg_id / taskid）
 *          └─ uuid_generate() × 2 + msg_begin() / msg_send()    收到的短报文 -> 组 JSON 上报 42214 / 42210
 *   组包公共步骤：msg_begin() 写报文头 -> json_add_*() 写 content -> msg_send() 收尾并发送
 *   时间：uptime_ms()（64 位，不回绕）+ s_time_base_ms -> now_unix()
 *   日志：收到的原文、json_check() 的检查结果、解析出的字段在 handle_json() 打，42212 的正文在
 *         forward_text() 打，发出的原文在 msg_send() 打；log_wanted() 决定心跳要不要打
 *   term_post()：由 BD / TT 任务调用，把消息放进本任务的队列
 */
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "app_config.h"
#include "bd_task.h"
#include "dev_status.h"
#include "json.h"
#include "log.h"
#include "msg_types.h"
#include "term_task.h"
#include "tt_task.h"
#include "udp_link.h"
#include "uuid.h"
#include "wdg.h"

#define TERM_POLL_MS        20
#define TERM_QUEUE_LEN      4
#define TERM_POST_WAIT_MS   100
#define TERM_RX_MAX         6144    /* 终端 JSON 最大长度（中文写成 \uXXXX 时会变长） */
#define TERM_TX_MAX         4096

/*
 * 设备表：北斗、天通走同一套短报文流程，只是消息号和提交函数不同。
 * forward_text() 用 submit 把请求交给对应的设备任务；handle_device_msg() 用 result / recv
 * 选择回给终端的消息号。以后接入新设备，在 dev_id_t 里加一项、这里加一行即可。
 */
static const struct {
    const char *name;
    uint16_t    result;             /* 本机 -> 终端：发送结果 */
    uint16_t    recv;               /* 本机 -> 终端：收到的短报文 */
    bool      (*submit)(const text_msg_t *req);
} k_dev[DEV_COUNT] = {
    [DEV_BD] = { "BD", MSG_BD_MESSAGE_REPLY, MSG_BD_MESSAGE_RECV, bd_submit },
    [DEV_TT] = { "TT", MSG_TT_MESSAGE_REPLY, MSG_TT_MESSAGE_RECV, tt_submit },
};

static udp_link_t    s_link;                /* 终端链路：本机 CFG_TERM_LOCAL_PORT <-> 终端 */
static QueueHandle_t s_queue;               /* TERM 队列：设备任务交来的 text_msg_t */
static uint64_t      s_time_base_ms;        /* unix 毫秒时间 = s_time_base_ms + 本机运行毫秒数 */
static text_msg_t    s_msg;                 /* 终端请求 / 设备消息，同一时刻只处理一条 */
static char          s_rx[TERM_RX_MAX + 1]; /* 接收缓冲，多 1 字节放结束符 */
static char          s_tx[TERM_TX_MAX];     /* 发送缓冲，所有回复共用 */
static unsigned      s_tx_type;             /* s_tx 里正在组的报文的消息号，msg_send() 打日志用 */

/* ---------------------------- 日志 ---------------------------- */

/*
 * 这个消息号的报文要不要打印原文和解析结果
 *
 * CFG_LOG_TERM_HEARTBEAT 为 0 时，心跳请求 42201 和心跳回复 42202 不打印（心跳很频繁，
 * 打出来会淹没其他日志），其他报文照常打印。
 *
 * 参数：type —— 消息号
 * 返回：true 要打印
 * 调用：handle_json()、msg_send()
 */
static bool log_wanted(unsigned type)
{
    return CFG_LOG_TERM_HEARTBEAT || (type != MSG_HEARTBEAT_REQUEST && type != MSG_HEARTBEAT_REPLY);
}

/* ---------------------------- 时间 ---------------------------- */

/*
 * 本机运行毫秒数，64 位，不会回绕
 *
 * FreeRTOS 的 tick 计数是 32 位（1 tick = 1ms 时约 49.7 天回绕一次）。vTaskSetTimeOutState()
 * 能在临界区里同时取到 tick 计数和内核记录的回绕次数，两者拼成 64 位 tick 数，再换算成毫秒。
 *
 * 返回：调度器启动以来的毫秒数
 * 调用：sync_time()、now_unix()
 */
static uint64_t uptime_ms(void)
{
    TimeOut_t t;
    uint64_t  ticks;

    vTaskSetTimeOutState(&t);
    ticks = ((uint64_t)(uint32_t)t.xOverflowCount << 32) | (uint64_t)t.xTimeOnEntering;
    return ticks * 1000U / configTICK_RATE_HZ;
}

/*
 * 用终端报文里的 timestamp 校准本机时间
 *
 * 本机没有 RTC 时钟源，只记录差值 s_time_base_ms = unix 毫秒时间 - 本机运行毫秒数，
 * 之后 now_unix() 由它推算。每条带 timestamp 的终端报文都会重新校准一次。
 * 终端发的是秒（1.docx 示例 1748990097）还是毫秒都认：小于 10^11 的按秒处理，先乘 1000
 * （秒级时间戳要到公元 5000 年以后才会这么大）。
 *
 * 参数：js —— 终端发来的整段 JSON 文本
 * 调用：handle_json()，每条终端报文都调用；没有 timestamp 或为 0 时不做任何改动
 */
static void sync_time(const char *js)
{
    uint64_t ts;

    if (!json_get_uint(js, "timestamp", &ts) || ts == 0U) {
        return;
    }
    if (ts < 100000000000ULL) {
        ts *= 1000U;
    }
    s_time_base_ms = ts - uptime_ms();
}

/*
 * 当前 unix 时间，用作所有发给终端的报文的 timestamp 字段
 *
 * 单位由 app_config.h 的 CFG_TERM_TS_MS 决定：1 毫秒（和 09 测试记录 test.txt 一致，13 位），
 * 0 秒（和 1.docx 示例一致，10 位）。还没收到过终端报文时基准是 0，返回的就是开机时长。
 *
 * 调用：msg_begin()
 */
static uint64_t now_unix(void)
{
    uint64_t ms = s_time_base_ms + uptime_ms();

#if CFG_TERM_TS_MS
    return ms;
#else
    return ms / 1000U;
#endif
}

/* -------------------------- 组包发送 -------------------------- */

/*
 * 开始组一条发给终端的报文：写报文头，然后打开 content 对象
 *
 * 写出的内容：{"msg_id":"…","timestamp":…,"msg_type":"…","content":{
 * 调用者接着用 json_add_*() 往 content 里写字段，最后调用 msg_send() 收尾并发送。
 * msg_type 按协议写成字符串（如 "42202"），timestamp 取 now_unix()。
 * 所有报文共用发送缓冲 s_tx，所以只能在 TERM 任务里、一条一条地组。
 *
 * 参数：w      —— JSON 写入器（调用者在栈上定义）
 *       msg_id —— 报文 ID
 *       type   —— 消息号，如 MSG_HEARTBEAT_REPLY
 * 调用：reply_status()、reply_heartbeat()、handle_device_msg()
 */
static void msg_begin(json_writer_t *w, const char *msg_id, unsigned type)
{
    char t[8];

    snprintf(t, sizeof(t), "%u", type);
    s_tx_type = type;
    json_begin(w, s_tx, sizeof(s_tx));
    json_add_str(w, "msg_id", msg_id);
    json_add_u64(w, "timestamp", now_unix());
    json_add_str(w, "msg_type", t);
    json_add_obj(w, "content");
}

/*
 * 结束并发送一条报文：关闭 content 和最外层对象，经 UDP 发给终端
 *
 * 组包过程中发送缓冲 s_tx（4KB）放不下时，json_end() 返回 -1，此时整条丢弃并打日志，
 * 不会发出半截 JSON。
 * CFG_LOG_TERM_RAW 为 1 时，发出后把整条 JSON 原文打在日志里（"TERM: tx 42213, N bytes: {...}"）；
 * 先发后打，免得打日志的时间（每 1KB 约 90ms）拖慢回复。
 *
 * 参数：w —— msg_begin() 初始化过、已写完 content 的写入器
 * 调用：reply_status()、reply_heartbeat()、handle_device_msg()
 */
static void msg_send(json_writer_t *w)
{
    int  len;
    bool ok;

    json_end_obj(w);                        /* content */
    len = json_end(w);                      /* 最外层 */
    if (len < 0) {
        LOG("TERM: message %u too long, dropped", s_tx_type);
        return;
    }
    ok = udp_link_send(&s_link, s_tx, len);
#if CFG_LOG_TERM_RAW
    if (log_wanted(s_tx_type)) {
        log_text(s_tx, (size_t)len, "TERM: tx %u%s, %d bytes", s_tx_type, ok ? "" : " FAILED", len);
    }
#else
    (void)ok;
#endif
}

/*
 * 通用回复：content = { "taskid", "status", 失败原因 }
 *
 * 失败原因的键名是 msg_types.h 的 KEY_REPLY_ERRMSG（按 1.docx 为 "errormsg"）。
 * 用于 42213 / 42209 发送结果、请求提交失败时的立即回复、本机不支持的请求（请求号 + 1）。
 *
 * 参数：type   —— 回复的消息号
 *       msg_id —— 报文 ID（回复请求时原样带回请求里的）
 *       taskid —— 任务 ID（同上）
 *       status —— 0 成功，1 失败
 *       err    —— 失败原因，成功时为空串
 * 调用：handle_json()、forward_text()、handle_device_msg()
 */
static void reply_status(unsigned type, const char *msg_id, const char *taskid, int status, const char *err)
{
    json_writer_t w;

    msg_begin(&w, msg_id, type);
    json_add_str(&w, "taskid", taskid);
    json_add_int(&w, "status", status);
    json_add_str(&w, KEY_REPLY_ERRMSG, err);
    msg_send(&w);
}

/*
 * 回复 42202 心跳：设备字段全部取自设备状态总表 dev_status 的一份快照
 *
 * content 结构：
 *   taskid / status（固定 0）/ errormsg（空）
 *   taskstate  ：有在途短报文时为 {taskid, sendprogress}，否则是空对象 {}
 *   Config.TT  ：status / msg / SQ / maxlength
 *   Config.BD  ：status / msg / CD / SQ / maxlength
 * 协议里 SQ、maxlength 是字符串，status、CD 是数字，所以前两个先 snprintf 成文字再写。
 * 先用 dev_status_read() 拷一份快照再组包，组包期间不占用状态表的锁。
 *
 * 参数：msg_id / taskid —— 原样带回心跳请求里的
 * 调用：handle_json()，收到 42201 时
 */
static void reply_heartbeat(const char *msg_id, const char *taskid)
{
    dev_status_t  st;
    json_writer_t w;
    char          sq[8];
    char          len[8];

    dev_status_read(&st);
    msg_begin(&w, msg_id, MSG_HEARTBEAT_REPLY);
    json_add_str(&w, "taskid", taskid);
    json_add_int(&w, "status", 0);
    json_add_str(&w, "errormsg", "");

    json_add_obj(&w, "taskstate");          /* 有任务才填，没有就是空对象 */
    if (st.task.taskid[0] != '\0') {
        json_add_str(&w, "taskid", st.task.taskid);
        json_add_int(&w, "sendprogress", st.task.progress);
    }
    json_end_obj(&w);

    json_add_obj(&w, "Config");

    json_add_obj(&w, "TT");
    snprintf(sq, sizeof(sq), "%u", (unsigned)st.tt.sq);
    snprintf(len, sizeof(len), "%u", (unsigned)st.tt.maxlen);
    json_add_int(&w, "status", st.tt.status);
    json_add_str(&w, "msg", st.tt.msg);
    json_add_str(&w, "SQ", sq);
    json_add_str(&w, "maxlength", len);
    json_end_obj(&w);

    json_add_obj(&w, "BD");
    snprintf(sq, sizeof(sq), "%u", (unsigned)st.bd.sq);
    snprintf(len, sizeof(len), "%u", (unsigned)st.bd.maxlen);
    json_add_int(&w, "status", st.bd.status);
    json_add_str(&w, "msg", st.bd.msg);
    json_add_int(&w, "CD", st.bd.cd);
    json_add_str(&w, "SQ", sq);
    json_add_str(&w, "maxlength", len);
    json_end_obj(&w);

    json_end_obj(&w);                       /* Config */
    msg_send(&w);
}

/* -------------------------- 业务处理 -------------------------- */

/*
 * 处理 42212（北斗）/ 42208（天通）文本传输请求：取出收信方和正文，交给设备任务
 *
 * 步骤：
 *   1. 在 s_msg 里填好 TEXT_SEND 请求，msg_id / taskid 原样保存，设备回执时带回；
 *   2. 从 content 取 target_id（收信方号码）和 msg（正文，\uXXXX 转义已还原成 UTF-8）；
 *   3. 调用设备表里的 submit：北斗是 bd_submit()（放入 BD 队列，不等待），
 *      天通是 tt_submit()（未接入，固定返回 false）。队列按值拷贝，s_msg 随后可以复用。
 * 任何一步失败都立即回 "请求号 + 1"、status = 1 和失败原因，终端不用等超时；
 * 提交成功时这里不回复，等设备任务用 term_post() 交回结果后，由 handle_device_msg() 回复。
 * 日志：target_id、msg 都取到后，打印解析结果（收信方号码 + 还原成 UTF-8 的完整正文），
 * 可以直接核对正文里的中文、\uXXXX 转义有没有还原对。
 *
 * 参数：dev    —— DEV_BD / DEV_TT
 *       js     —— 整段请求 JSON
 *       msg_id / taskid —— handle_json() 已从请求中取出（缺失时已补齐）
 * 调用：handle_json()
 */
static void forward_text(dev_id_t dev, const char *js, const char *msg_id, const char *taskid)
{
    const char *err = NULL;

    s_msg.kind   = TEXT_SEND;
    s_msg.dev    = dev;
    s_msg.status = 0;
    snprintf(s_msg.msg_id, sizeof(s_msg.msg_id), "%s", msg_id);
    snprintf(s_msg.taskid, sizeof(s_msg.taskid), "%s", taskid);

    if (!json_get_str(js, "target_id", s_msg.peer, sizeof(s_msg.peer))) {
        err = "missing or invalid target_id";
    } else if (!json_get_str(js, "msg", s_msg.text, sizeof(s_msg.text))) {
        err = "missing msg or msg too long";
    } else {
        log_text(s_msg.text, strlen(s_msg.text), "TERM: %s text parsed: target_id \"%s\", msg %u bytes UTF-8",
                 k_dev[dev].name, s_msg.peer, (unsigned)strlen(s_msg.text));
        if (!k_dev[dev].submit(&s_msg)) {
            err = "device busy or not available";
        }
    }

    if (err != NULL) {
        LOG("TERM: %s text task %s rejected: %s", k_dev[dev].name, taskid, err);
        reply_status(k_dev[dev].result, msg_id, taskid, 1, err);
        return;
    }
    LOG("TERM: %s text task %s -> %s, queued", k_dev[dev].name, taskid, s_msg.peer);
}

/*
 * 处理终端发来的一条 JSON 报文：取公共字段、校时，再按 msg_type 分发
 *
 * 公共字段：
 *   msg_type —— 必须有，数字或数字字符串都认，没有就丢弃整条报文；
 *   msg_id / taskid —— 终端下发的请求一般都带，原样带回；缺哪个就用 uuid_generate() 补一个
 *                      标准 UUID（两个各自独立生成），保证回复里不为空；
 *   timestamp —— 交给 sync_time() 校准本机时间。
 * json_get_*() 是在整段文本里按键名查找的，所以 content 里的 taskid 也能直接取到。
 *
 * 分发：
 *   42201                                 心跳请求  -> reply_heartbeat() 回 42202
 *   42212 / 42208                         文本传输  -> forward_text() 交给北斗 / 天通任务
 *   42215 / 42211 / 42219 / 42225         终端对本机上报的确认 -> 只打日志，不重发
 *   42204 / 42206 / 42216 / 42220 / 42222 / 42226   本机不支持 -> 回请求号 + 1、status = 1
 *   其他                                  打日志后丢弃
 *
 * 日志（用来确认终端的包收到了、解析对了）：
 *   1. "TERM: rx N bytes: {...}"     收到的原文（CFG_LOG_TERM_RAW）；
 *   2. "TERM: warning: ..."          包不是完整的 JSON 对象（json_check()）或占满了接收缓冲（可能被截断），
 *                                    照样继续解析，但取到的字段可能不全；
 *   3. "TERM: parsed msg_type ..."   解析出的公共字段；msg_id / taskid 是本机补的会注明 generated，
 *                                    没有 timestamp 写 none；
 *   之后各分支再打自己的结果（如 42212 的 "text parsed"）。
 *   CFG_LOG_TERM_HEARTBEAT 为 0 时心跳请求不打 1、3（出错的 2 照打）。
 *
 * 参数：js  —— 以 '\0' 结尾的 JSON 文本（即 s_rx）
 *       len —— 收到的字节数（包里有 0x00 时比 strlen(js) 大）
 * 调用：term_task() 主循环，每收到一个 UDP 包调用一次
 */
static void handle_json(const char *js, int len)
{
    char        msg_id[APP_ID_LEN];
    char        taskid[APP_ID_LEN];
    char        ts[24];                     /* timestamp 原文，只用于日志 */
    uint64_t    value;
    unsigned    type    = 0;
    bool        has_type;
    bool        new_id;
    bool        new_task;
    bool        verbose;
    const char *bad;

    has_type = json_get_uint(js, "msg_type", &value);
    if (has_type) {
        type = (unsigned)value;
    }
    verbose = !has_type || log_wanted(type);

#if CFG_LOG_TERM_RAW
    if (verbose) {
        log_text(js, (size_t)len, "TERM: rx %d bytes", len);
    }
#endif
    bad = json_check(js, (size_t)len);
    if (bad != NULL) {
        LOG("TERM: warning: not a valid JSON object: %s", bad);
    }
    if (len >= TERM_RX_MAX) {
        LOG("TERM: warning: packet fills the %d-byte rx buffer, may be truncated", TERM_RX_MAX);
    }
    if (!has_type) {
        LOG("TERM: parse failed: no msg_type, dropped: %.60s", js);
        return;
    }

    new_id = !json_get_str(js, "msg_id", msg_id, sizeof(msg_id));
    if (new_id) {
        uuid_generate(msg_id);
    }
    new_task = !json_get_str(js, "taskid", taskid, sizeof(taskid));
    if (new_task) {
        uuid_generate(taskid);
    }
    if (!json_get_str(js, "timestamp", ts, sizeof(ts))) {
        snprintf(ts, sizeof(ts), "none");
    }
    sync_time(js);
    if (verbose) {
        LOG("TERM: parsed msg_type %u, msg_id %s%s, taskid %s%s, timestamp %s", type,
            msg_id, new_id ? " (missing, generated)" : "", taskid, new_task ? " (missing, generated)" : "", ts);
    }

    switch (type) {
    case MSG_HEARTBEAT_REQUEST:
        reply_heartbeat(msg_id, taskid);
        break;

    case MSG_BD_MESSAGE_SEND:
        forward_text(DEV_BD, js, msg_id, taskid);
        break;

    case MSG_TT_MESSAGE_SEND:
        forward_text(DEV_TT, js, msg_id, taskid);
        break;

    case MSG_BD_MESSAGE_RECV_ACK:           /* 终端对本机上报的确认 */
    case MSG_TT_MESSAGE_RECV_ACK:
    case MSG_TT_CALL_IN_ACK:
    case MSG_TT_CALL_ENDED_ACK:
        value = 0;
        json_get_uint(js, "status", &value);
        LOG("TERM: ack %u for task %s, status %u", type, taskid, (unsigned)value);
        break;

    case MSG_VHF_CHANNEL_SET:               /* 本机暂不支持的请求：直接回失败，免得终端一直等 */
    case MSG_MESH_POWER_SET:
    case MSG_TT_CALL_OUT:
    case MSG_TT_CALL_ACCEPT:
    case MSG_TT_CALL_REJECT:
    case MSG_TT_POWER_SET:
        reply_status(type + 1U, msg_id, taskid, 1, "not supported");
        break;

    default:
        LOG("TERM: unknown msg_type %u", type);
        break;
    }
}

/*
 * 处理设备任务（北斗 / 天通）通过 term_post() 交来的一条消息，组成 JSON 发给终端
 *
 *   TEXT_RESULT：短报文发送结果 -> 42213（北斗）/ 42209（天通）
 *                msg_id / taskid 原样带回终端当初请求里的，text 是失败原因（成功为空）
 *   TEXT_RECV  ：设备收到的短报文 -> 42214（北斗）/ 42210（天通）
 *                属于本机主动上报，msg_id 和 taskid 各用 uuid_generate() 生成一个标准 UUID
 *                （和 09 测试记录 test.txt 里的主动上报一样，两个 ID 互相独立）；
 *                content = { taskid, target_id（发信方号码）, msg（UTF-8 正文） }
 * 终端随后回的确认（42215 / 42211）由 handle_json() 处理，只打日志。
 *
 * 参数：m —— 从 TERM 队列取出的消息；dev 越界或 kind 不认识时忽略
 * 调用：term_task() 主循环，每圈把 TERM 队列取空
 */
static void handle_device_msg(const text_msg_t *m)
{
    json_writer_t w;
    char          msg_id[UUID_STR_SIZE];
    char          taskid[UUID_STR_SIZE];

    if (m->dev >= DEV_COUNT) {
        return;
    }
    if (m->kind == TEXT_RESULT) {
        LOG("TERM: %s result of task %s -> terminal %u: status %d%s%s", k_dev[m->dev].name, m->taskid,
            k_dev[m->dev].result, m->status, (m->text[0] != '\0') ? ", " : "", m->text);
        reply_status(k_dev[m->dev].result, m->msg_id, m->taskid, m->status, m->text);
        return;
    }
    if (m->kind != TEXT_RECV) {
        return;
    }

    uuid_generate(msg_id);
    uuid_generate(taskid);
    msg_begin(&w, msg_id, k_dev[m->dev].recv);
    json_add_str(&w, "taskid", taskid);
    json_add_str(&w, "target_id", m->peer);
    json_add_str(&w, "msg", m->text);
    msg_send(&w);
    LOG("TERM: %s text from %s reported, msg %s task %s", k_dev[m->dev].name, m->peer, msg_id, taskid);
}

/* ------------------------------ 任务 ------------------------------ */

/*
 * 创建 TERM 队列：深度 4，每项一个 text_msg_t（约 3.2KB，整个队列约 13KB，从 RTOS 堆分配）
 *
 * BD / TT 任务在 main() 里就创建了，一启动就可能调用 term_post()，所以队列在调度器启动前建好。
 * 创建失败（堆不够）时 configASSERT 关中断死循环，由硬件看门狗复位。
 * 调用：main()
 */
void term_init(void)
{
    s_queue = xQueueCreate(TERM_QUEUE_LEN, sizeof(text_msg_t));
    configASSERT(s_queue != NULL);
}

/*
 * 设备任务把一条消息交给终端任务：放入 TERM 队列（按值拷贝）
 *
 * 队列满时最多等 100ms —— TERM 任务每 20ms 至少取空一次队列，正常不会等这么久；
 * 仍然放不进去就返回 false，由调用方打日志，这条结果 / 短报文就丢失了。
 * 网线没插、TERM 任务还没创建时，队列存满 4 条后的消息也会这样丢掉。
 *
 * 参数：msg —— kind 为 TEXT_RESULT（发送结果）或 TEXT_RECV（收到的短报文），dev 标明来源设备
 * 返回：true 已入队；false 队列满
 * 调用：bd_task.c 的 finish_send()、on_tci()，运行在 BD 任务里（天通接入后也用它）
 */
bool term_post(const text_msg_t *msg)
{
    return xQueueSend(s_queue, msg, pdMS_TO_TICKS(TERM_POST_WAIT_MS)) == pdPASS;
}

/*
 * 终端任务主函数（优先级 4，由 start_task() 在网络就绪后创建）
 *
 * 启动：先向看门狗登记，再打开终端 UDP 链路（本机 CFG_TERM_LOCAL_PORT <-> 终端
 *       CFG_TERM_IP:CFG_TERM_PORT，接收超时 20ms）；打开失败每秒重试，期间照常喂狗。
 * 主循环每一圈：
 *   1. wdg_feed() 报平安；
 *   2. udp_link_recv() 最多等 20ms 收一个包，收到就补上结束符，交给 handle_json() 处理；
 *   3. 非阻塞地把 TERM 队列取空，每条交给 handle_device_msg() 组 JSON 发给终端。
 * 一圈最长约 20ms，所以设备任务交来的消息最多延迟约 20ms 就会发给终端。
 *
 * 参数：arg —— 未使用
 */
void term_task(void *arg)
{
    int wdg = wdg_register("TERM");

    (void)arg;
    while (!udp_link_open(&s_link, "TERM", CFG_TERM_LOCAL_PORT, CFG_TERM_IP, CFG_TERM_PORT, TERM_POLL_MS)) {
        wdg_feed(wdg);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        int n;

        wdg_feed(wdg);
        n = udp_link_recv(&s_link, s_rx, TERM_RX_MAX);
        if (n > 0) {
            s_rx[n] = '\0';
            handle_json(s_rx, n);
        }
        while (xQueueReceive(s_queue, &s_msg, 0) == pdPASS) {
            handle_device_msg(&s_msg);
        }
    }
}
