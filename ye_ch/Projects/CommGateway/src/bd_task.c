/*
 * bd_task.c —— 北斗任务（北斗设备经 RS422 串口接在 USART6 上）
 *
 * 主循环（每圈最多等 20ms 收串口数据）：
 *   1. 取串口收到的数据，断句后按语句类型分发（PWI / GSS / ICP / FKI / TCI）
 *   2. 在途短报文等回执超时就判失败
 *   3. 没有在途短报文时，从队列取下一条终端请求发出去
 *   4. 还没读到卡就定时发 ICR；每秒刷新一次 SQ / status
 *
 * 函数调用关系（本文件的函数全部运行在 BD 任务里，bd_submit() 除外）：
 *   bd_task()                                   任务主循环
 *     ├─ serial_read()                          从串口环形缓冲区取数据（serial.c）
 *     ├─ bd_framer_feed() ──回调──> on_line()   把字节流断句，每句调用一次 on_line()
 *     │    └─ bd_parse() 后记下收到时刻（判断串口是否断线），再查 k_handlers[] 分发：
 *     │         ├─ on_pwi() / on_gss() -> sig_update()      记录波束数 / 卫星数
 *     │         ├─ on_icp()  -> dev_status                  记录卡信息，算 CD / maxlength
 *     │         ├─ on_fki()  -> finish_send()               TCQ 回执：成功 / 失败
 *     │         └─ on_tci()  -> term_post()                 收到的短报文交给终端任务
 *     ├─ finish_send()                          在途短报文等 FKI 超时
 *     ├─ start_send()                           取下一条请求：gbk_from_utf8 -> bd_mixed_encode
 *     │                                         -> bd_build($BDTCQ) -> serial_write
 *     ├─ query_card()                           还没读到卡：每 5s 发一次 $CCICR
 *     └─ update_signal() -> sig_level()         每 1s：串口断线判异常，否则两路信号折算成 SQ / status
 *   bd_submit()：由 TERM 任务调用，把发报请求放进本任务的 BD 队列
 *   日志：on_line() 打印收到的每条语句原文（PWI / GSS 默认不打，见 is_periodic()），log_tx() 打印
 *         发出的 TCQ / ICR 原文；on_fki() / on_tci() 打印解析结果；update_signal() 在串口有字节
 *         进来却凑不出有效语句时提示查波特率和接线。开关在 app_config.h 的 CFG_LOG_BD_*
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "app_config.h"
#include "bd_proto.h"
#include "bd_task.h"
#include "dev_status.h"
#include "gbk.h"
#include "log.h"
#include "serial.h"
#include "term_task.h"
#include "wdg.h"

#define BD_POLL_MS          20      /* 串口没数据时最多等这么久，即主循环最长周期 */
#define BD_RX_CHUNK         256     /* 每圈最多从串口取这么多字节（38400 波特率下 20ms 只来约 80 字节） */
#define BD_TX_WAIT_MS       1500    /* 串口上一包还没发完时最多等多久（要小于看门狗的 3 秒） */
#define BD_SQ_PERIOD_MS     1000    /* SQ 刷新周期 */
#define BD_QUEUE_LEN        2       /* 待发短报文队列深度 */
#define BD_GBK_MAX          2000    /* 一条短报文最多 2000 字节 GBK（等级 5：1000 个汉字） */
#define BD_CODE_MIXED       3       /* 编码类型 3：混编 */
#define BD_BEAMS_MAX        16      /* PWI 里每种体制的波束锁定数上限（3.md 表 3） */
#define SQ_MAX              4

/* 每秒自动输出的语句（PWI / GSS）的解析结果，只在 CFG_LOG_BD_PERIODIC 为 1 时打印，否则每秒好几行 */
#if CFG_LOG_BD_PERIODIC
#define LOG_PERIODIC(...)   LOG(__VA_ARGS__)
#else
#define LOG_PERIODIC(...)   ((void)0)
#endif

/* 心跳里北斗 status 的取值（1.docx：0 在线，1 离线，2 异常） */
#define BD_ONLINE           0
#define BD_OFFLINE          1       /* 串口正常，但 SQ 为 0（没有信号） */
#define BD_FAULT            2       /* 串口超过 CFG_BD_LINK_TIMEOUT_MS 没收到有效语句（线断、设备没电） */

/* 通信长度等级 1~5 对应的可发字数（design.txt），CN 汉字数、EN 字符数 */
#define CN_MAX_COUNT_LEVEL1 (43   - 1)
#define CN_MAX_COUNT_LEVEL2 (114  - 1)
#define CN_MAX_COUNT_LEVEL3 (323  - 1)
#define CN_MAX_COUNT_LEVEL4 (498  - 1)
#define CN_MAX_COUNT_LEVEL5 (1000 - 1)
#define EN_MAX_COUNT_LEVEL1 (86   - 4)
#define EN_MAX_COUNT_LEVEL2 (228  - 4)
#define EN_MAX_COUNT_LEVEL3 (646  - 4)
#define EN_MAX_COUNT_LEVEL4 (996  - 4)
#define EN_MAX_COUNT_LEVEL5 (2000 - 4)

/* 等级 -> 字数查表，下标 = 等级 - 1（on_icp() 使用） */
static const uint16_t k_len_cn[5] = {
    CN_MAX_COUNT_LEVEL1, CN_MAX_COUNT_LEVEL2, CN_MAX_COUNT_LEVEL3, CN_MAX_COUNT_LEVEL4, CN_MAX_COUNT_LEVEL5
};
static const uint16_t k_len_en[5] = {
    EN_MAX_COUNT_LEVEL1, EN_MAX_COUNT_LEVEL2, EN_MAX_COUNT_LEVEL3, EN_MAX_COUNT_LEVEL4, EN_MAX_COUNT_LEVEL5
};

/* 一路信号数据（PWI 波束数或 GSS 卫星数）：计数值和收到的时刻 */
typedef struct {
    bool       seen;            /* 至少收到过一次 */
    uint8_t    count;           /* 波束 / 卫星数量 */
    TickType_t at;              /* 最近一次收到的 tick，用来判断数据是否过期 */
} sig_src_t;

/*
 * 北斗每秒自动输出的语句类型：CFG_LOG_BD_PERIODIC 为 0 时不打印它们的原文。
 * 设备如果还每秒输出别的语句（如 SNR、ZTI）、刷屏太厉害，把类型加在这里即可。
 */
static const char *const k_periodic[] = { "PWI", "GSS" };

static bd_framer_t   s_framer;              /* 断句器，跨两次读串口保存半条语句 */
static QueueHandle_t s_queue;               /* BD 队列：终端任务提交的待发短报文 */
static bool          s_card_ok;             /* 已读到 IC 卡信息，不再发 ICR */
static sig_src_t     s_beams;               /* PWI：新体制出站波束锁定数 */
static sig_src_t     s_sats;                /* GSS：可用卫星数 */
static TickType_t    s_rx_at;               /* 最近一次收到有效语句的时刻（初值为任务启动时刻），判断串口是否断线 */
static uint32_t      s_rx_bytes;            /* 串口累计收到的字节数（回绕无妨，只用差值） */
static uint32_t      s_rx_bytes_mark;       /* 最近一次收到有效语句（或提示过乱码）时的 s_rx_bytes */
static TickType_t    s_junk_at;             /* 最近一次提示"有数据但没有有效语句"的时刻 */

/* 在途短报文：发出 TCQ 后等 FKI，同一时间只有一条 */
static struct {
    bool       active;                      /* true：TCQ 已发出，正在等 FKI */
    TickType_t deadline;                    /* 等 FKI 的截止 tick */
    char       msg_id[APP_ID_LEN];          /* 终端请求的 msg_id / taskid，回执时带回 */
    char       taskid[APP_ID_LEN];
} s_pending;

static text_msg_t s_req;                    /* 正在处理的终端请求 */
static text_msg_t s_out;                    /* 交给终端的消息 */
static char       s_rx[BD_RX_CHUNK];        /* 从串口取出的一段数据 */
static char       s_tx[BD_LINE_MAX];        /* 组好的待发语句（TCQ / ICR） */
static uint8_t    s_gbk[BD_GBK_MAX];        /* GBK 正文：发送时是编码前，接收时是解码后 */
static char       s_hex[2 + BD_GBK_MAX * 2 + 1];    /* 混编文本："A4" + 每字节两位十六进制 + 结束符 */

/*
 * 判断 tick 时刻 when 是否已到（now >= when），tick 计数回绕时也正确
 *
 * 原理：两个 tick 相减后按有符号数看，只要两者相差不超过 2^31 tick（约 24.8 天），
 * 差值的正负就能正确反映先后，不受 32 位计数从 0xFFFFFFFF 回到 0 的影响。
 *
 * 返回：true 已到点或已过点
 * 调用：bd_task() 主循环（FKI 超时、ICR 重发、SQ 刷新三个定时判断）
 */
static bool time_reached(TickType_t now, TickType_t when)
{
    return (int32_t)(now - when) >= 0;
}

/*
 * 更新心跳里的 taskstate（正在发送的短报文任务及进度），加锁写入设备状态总表
 *
 * 进度约定：start_send() 开始时 0，TCQ 交给串口后 50，finish_send() 结束时清空
 * （taskid 置空串，心跳里的 taskstate 变回空对象）。
 *
 * 参数：taskid   —— 任务 ID，空串表示没有任务
 *       progress —— 发送进度 0~100
 * 调用：start_send()、finish_send()
 */
static void set_task_state(const char *taskid, uint8_t progress)
{
    dev_status_t *st = dev_status_lock();

    snprintf(st->task.taskid, sizeof(st->task.taskid), "%s", taskid);
    st->task.progress = progress;
    dev_status_unlock();
}

/* ------------------------------ 日志 ------------------------------ */

/*
 * 打印发给北斗的一条语句原文（CFG_LOG_BD_RAW 为 1 时），末尾的 \r\n 不打印
 *
 * 例：BD: tx: $CCICR,0,00*68
 * 参数：s / len —— bd_build() 组好的语句及长度；len <= 0 时只打 "(empty)"
 * 调用：start_send()（TCQ）、query_card()（ICR）
 */
static void log_tx(const char *s, int len)
{
#if CFG_LOG_BD_RAW
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    log_text(s, (len > 0) ? (size_t)len : 0U, "BD: tx");
#else
    (void)s;
    (void)len;
#endif
}

/*
 * 判断一条语句是不是北斗每秒自动输出的（类型在 k_periodic 表里），在 bd_parse() 之前调用
 *
 * 语句头是第一个逗号（或 '*'）之前的部分，如 "$BDPWI"，取最后 3 个字母和表比较，
 * 和 bd_parse() 认类型的方法一样，不看发送方标识。
 *
 * 参数：line —— 以 '$' 开头的一条语句（还没被 bd_parse() 改写）
 * 返回：true 是每秒自动输出的语句
 * 调用：on_line()，决定要不要打印原文
 */
static bool is_periodic(const char *line)
{
    size_t head = strcspn(line, ",*");      /* 含开头的 '$' */
    size_t i;

    if (head < 4U) {
        return false;
    }
    for (i = 0; i < sizeof(k_periodic) / sizeof(k_periodic[0]); i++) {
        if (memcmp(line + head - 3U, k_periodic[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------ 发送 ------------------------------ */

/*
 * 结束当前在途短报文：把发送结果交给终端任务，清除在途状态
 *
 * 组一条 TEXT_RESULT 消息（msg_id / taskid 取自 s_pending，即终端当初的请求），
 * 用 term_post() 交给终端任务，终端任务再回 42213。随后清掉 s_pending.active 和 taskstate，
 * 主循环下一圈就可以从 BD 队列取下一条请求。
 *
 * 参数：status —— 0 成功，1 失败
 *       err    —— 失败原因（成功时为空串），原样成为 42213 的 errormsg
 * 调用：on_fki()（收到回执）、bd_task()（等 FKI 超时）、start_send()（编码或发送失败）
 */
static void finish_send(int status, const char *err)
{
    s_out.kind    = TEXT_RESULT;
    s_out.dev     = DEV_BD;
    s_out.status  = status;
    s_out.peer[0] = '\0';
    snprintf(s_out.msg_id, sizeof(s_out.msg_id), "%s", s_pending.msg_id);
    snprintf(s_out.taskid, sizeof(s_out.taskid), "%s", s_pending.taskid);
    snprintf(s_out.text, sizeof(s_out.text), "%s", err);
    if (!term_post(&s_out)) {
        LOG("BD: terminal queue full, result of task %s lost", s_pending.taskid);
    }

    LOG("BD: task %s %s %s", s_pending.taskid, (status == 0) ? "sent" : "failed:", err);
    s_pending.active = false;
    set_task_state("", 0);
}

/*
 * 判断字符串是否非空且全部由数字组成，用来校验北斗收信方号码（target_id）
 *
 * 号码会原样拼进 TCQ 语句，含逗号、'*' 等字符会破坏语句格式，所以发送前先检查。
 *
 * 返回：true 非空且全是 '0'~'9'
 * 调用：start_send()
 */
static bool is_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

/*
 * 发出一条北斗短报文（TCQ），进入"等 FKI 回执"状态
 *
 * 步骤：
 *   1. 登记在途短报文 s_pending（保存 msg_id / taskid），taskstate 进度置 0；
 *   2. 校验收信方号码（必须全是数字）和正文（不能为空）；
 *   3. 正文 UTF-8 -> GBK（gbk_from_utf8）-> 混编 "A4" + 十六进制（bd_mixed_encode）；
 *   4. 组 $BDTCQ,收信方,频点,入站确认,3,混编数据,0*hh（bd_build），交给串口 DMA 发给北斗；
 *   5. 成功：FKI 截止时刻 = 现在 + 串口发送时间 + CFG_BD_FKI_TIMEOUT_MS，进度置 50；
 *      任一步失败：finish_send(1, 原因)，终端立即收到 42213 失败。
 * 串口发送时间 = 字节数 × 10 位 / 波特率，38400 下最长的 TCQ（约 4.1KB）要 1.1 秒，
 * 计时要从发完开始算，所以加上这一段。
 * 例："12一二" -> GBK 31 32 D2 BB B6 FE -> "A43132D2BBB6FE"
 *     -> $BDTCQ,1859836,2,2,3,A43132D2BBB6FE,0*0C
 *
 * 参数：req —— 终端请求（kind = TEXT_SEND），peer 是收信方号码，text 是 UTF-8 正文
 * 调用：bd_task() 主循环，仅在没有在途短报文、且从 BD 队列取到请求时调用
 */
static void start_send(const text_msg_t *req)
{
    const char *err     = NULL;
    int         gbk_len = 0;
    int         len     = 0;
    uint32_t    tx_ms;

    s_pending.active = true;
    snprintf(s_pending.msg_id, sizeof(s_pending.msg_id), "%s", req->msg_id);
    snprintf(s_pending.taskid, sizeof(s_pending.taskid), "%s", req->taskid);
    set_task_state(req->taskid, 0);

    if (!is_digits(req->peer)) {
        err = "invalid target_id";
    } else if (req->text[0] == '\0') {
        err = "empty msg";
    } else if ((gbk_len = gbk_from_utf8(req->text, s_gbk, sizeof(s_gbk))) < 0 ||
               bd_mixed_encode(s_gbk, (size_t)gbk_len, s_hex, sizeof(s_hex)) < 0) {
        err = "msg too long";
    } else if ((len = bd_build(s_tx, sizeof(s_tx), "BDTCQ,%s,%d,%d,%d,%s,0", req->peer,
                               CFG_BD_TCQ_FREQ, CFG_BD_TCQ_CONFIRM, BD_CODE_MIXED, s_hex)) < 0) {
        err = "msg too long";
    } else if (!serial_write(SERIAL_BD, s_tx, (size_t)len, BD_TX_WAIT_MS)) {
        err = "BD serial busy";
    }

    if (err != NULL) {
        finish_send(1, err);
        return;
    }
    tx_ms = (uint32_t)len * 10U * 1000U / CFG_BD_BAUDRATE;
    s_pending.deadline = xTaskGetTickCount() + pdMS_TO_TICKS(tx_ms + CFG_BD_FKI_TIMEOUT_MS);
    set_task_state(req->taskid, 50);
    LOG("BD: TCQ task %s -> %s, %d bytes GBK, %d bytes on serial", req->taskid, req->peer, gbk_len, len);
    log_tx(s_tx, len);
}

/* --------------------------- 语句处理 --------------------------- */

/*
 * 记录一路信号的最新计数值和收到时刻
 *
 * 参数：src   —— s_beams（PWI）或 s_sats（GSS）
 *       count —— 解析出来的数量，限制到 0~255 后存进 uint8_t
 * 调用：on_pwi()、on_gss()
 */
static void sig_update(sig_src_t *src, int count)
{
    src->count = (uint8_t)((count < 0) ? 0 : (count > 255) ? 255 : count);
    src->at    = xTaskGetTickCount();
    src->seen  = true;
}

/*
 * 处理 PWI（北斗每秒自动输出的波束信息）：取新体制出站波束锁定数，作为 SQ 的一路输入
 *
 * 格式（3.md 表 3，以 111.pdf 截图里的实际输出为准）：
 *   $BDPWI,时间,旧体制波束数,[旧体制每波束 3 个字段...],新体制波束数,[新体制每波束 4 个字段...]
 *   例：$BDPWI,012430.31,00,03,11,45,45,00,16,49,49,00,21,46,46,00*4B（旧体制 0 个，新体制 3 个）
 * 新体制波束数的位置不固定：argv[1] 是旧体制波束数 N，跳过 N × 3 个字段后，
 * argv[2 + N × 3] 才是新体制波束数。
 * 实测旧体制一直是 00，SQ 只用新体制波束锁定数。数值不在 0~16 说明字段没对上，不更新。
 *
 * CFG_LOG_BD_PERIODIC 为 1 时打印解析出的两个波束数，字段没对上时也打印原因。
 *
 * 参数：s —— bd_parse() 拆好的语句，argv[0] 是协议里的第 1 个字段（时间）
 * 调用：on_line()，经 k_handlers 表分发
 */
static void on_pwi(const bd_sentence_t *s)
{
    int old_beams;
    int new_beams;
    int idx;

    if (s->argc < 2) {
        LOG_PERIODIC("BD: PWI has only %d fields, ignored", s->argc);
        return;
    }
    old_beams = atoi(s->argv[1]);
    idx       = 2 + old_beams * 3;          /* 跳过旧体制波束，后面就是新体制波束数 */
    if (old_beams < 0 || old_beams > BD_BEAMS_MAX || idx >= s->argc) {
        LOG_PERIODIC("BD: PWI fields not as expected (old beams %d, %d fields), ignored", old_beams, s->argc);
        return;
    }
    new_beams = atoi(s->argv[idx]);
    LOG_PERIODIC("BD: PWI parsed: old beams %d, new beams %d", old_beams, new_beams);
    if (new_beams >= 0 && new_beams <= BD_BEAMS_MAX) {    /* 位置没对上就不更新 */
        sig_update(&s_beams, new_beams);
    }
}

/*
 * 处理 GSS（全球短报文模块每秒输出的卫星信息）：取可用卫星数，作为 SQ 的另一路输入
 *
 * 格式（design.txt）：$BDGSS,可用卫星数,入站卫星号,[每颗卫星：卫星号,信号强度,仰角,方位角]...
 * 只用第 1 个字段。区域短报文模块（如 HTQ206S）不输出 GSS，这一路就一直没有数据。
 *
 * 参数：s —— 拆好的语句
 * 调用：on_line()，经 k_handlers 表分发
 */
static void on_gss(const bd_sentence_t *s)
{
    if (s->argc >= 1) {
        LOG_PERIODIC("BD: GSS parsed: sats %d", atoi(s->argv[0]));
        sig_update(&s_sats, atoi(s->argv[0]));
    }
}

/*
 * 处理 ICP（读卡结果，对 $CCICR 的应答）：记录服务频度和通信长度等级，算出 CD / maxlength
 *
 * 用到的字段（3.md 表 6，编号从 1 开始，argv 下标 = 编号 - 1）：
 *    1 用户地址（本机卡号）
 *   13 区域服务能力标识（1 = 有权限）     14 区域服务频度（秒）
 *   15 通信长度等级 1~5
 *   16 全球标识
 *   17 全球短报文交互权限（1 = 有权限）   18 全球短报文服务频度（秒）
 * 规则（design.txt）：
 *   - 有权限就记对应的服务频度，没权限记 0；两个都存进 card，CD 暂以全球频度上报；
 *   - 长度等级查 k_len_cn / k_len_en 得到汉字数和字符数，maxlength 暂以汉字数上报；
 *     等级不在 1~5 时两个字数都为 0。
 * 写完置 s_card_ok，主循环不再发 ICR（卡信息只读一次）。
 *
 * 参数：s —— 拆好的语句；字段少于 18 个时忽略，等下次 ICR 重发
 * 调用：on_line()，经 k_handlers 表分发
 */
static void on_icp(const bd_sentence_t *s)
{
    bd_card_t     card;
    dev_status_t *st;
    int           level;

    if (s->argc < 18) {
        LOG("BD: ICP has only %d fields, ignored", s->argc);
        return;
    }

    memset(&card, 0, sizeof(card));
    card.valid = true;
    snprintf(card.id, sizeof(card.id), "%s", s->argv[0]);
    card.freq_region = (atoi(s->argv[12]) == 1) ? (uint16_t)atoi(s->argv[13]) : 0;    /* 无权限记 0 */
    card.freq_global = (atoi(s->argv[16]) == 1) ? (uint16_t)atoi(s->argv[17]) : 0;
    level = atoi(s->argv[14]);
    if (level >= 1 && level <= 5) {
        card.len_level = (uint8_t)level;
        card.len_cn    = k_len_cn[level - 1];
        card.len_en    = k_len_en[level - 1];
    }

    st = dev_status_lock();
    st->bd.card   = card;
    st->bd.cd     = card.freq_global;       /* 暂以全球短报文服务频度上传 */
    st->bd.maxlen = card.len_cn;            /* 暂以汉字上限上传 */
    dev_status_unlock();

    s_card_ok = true;
    LOG("BD: card %s, freq region %us global %us, level %d (cn %u, en %u)", card.id,
        card.freq_region, card.freq_global, level, card.len_cn, card.len_en);
}

/*
 * 安全地取第 i 个字段（下标从 0 开始），字段不存在时返回 "-"
 *
 * 只用于拼错误信息，FKI 字段不全时也不会越界。
 * 调用：on_fki()
 */
static const char *field(const bd_sentence_t *s, int i)
{
    return (i < s->argc) ? s->argv[i] : "-";
}

/*
 * 处理 FKI（发射申请反馈），即北斗对 TCQ 的回执。手册没给字段表，按 111.pdf 截图识别：
 *   $BDFKI,时间,指令名称,执行情况,失败原因,等待时间*hh
 *   成功：$BDFKI,032025,TCQ,Y,0,0*77
 *   失败：$BDFKI,032816,TCQ,N,1,7*6E   （2.3.3.4 入站频度控制的示例：原因 1 = 频度未到，7 = 还要等的秒数）
 * 其他失败原因代码手册里没有，原样带回终端。OFQ/EPQ/QRQ 等指令也会出 FKI，只认 TCQ 的。
 *
 * 处理：先打印解析出的各字段；字段不足 3 个、指令名不是 TCQ、或没有在途短报文时忽略（日志写明原因，
 *       "没有在途"多半是 FKI 来得比超时还晚，那条短报文已经按超时回过终端了）；
 *       执行情况为 'Y' -> finish_send(0, "")；
 *       否则失败原因为 1 -> "BD send too frequent, wait Ns"，其他 -> "BD rejected, FKI reason x, y"，
 *       再 finish_send(1, 原因)。
 *
 * 参数：s —— 拆好的语句
 * 调用：on_line()，经 k_handlers 表分发
 */
static void on_fki(const bd_sentence_t *s)
{
    char err[64];

    LOG("BD: FKI parsed: cmd %s, result %s, reason %s, wait %s",
        field(s, 1), field(s, 2), field(s, 3), field(s, 4));
    if (s->argc < 3) {
        LOG("BD: FKI has only %d fields, ignored", s->argc);
        return;
    }
    if (strcmp(s->argv[1], "TCQ") != 0) {
        LOG("BD: FKI is for %s, not TCQ, ignored", s->argv[1]);
        return;
    }
    if (!s_pending.active) {
        LOG("BD: FKI for TCQ but no TCQ is waiting (came after timeout?), ignored");
        return;
    }
    if (s->argv[2][0] == 'Y') {
        finish_send(0, "");
        return;
    }

    if (strcmp(field(s, 3), "1") == 0) {
        snprintf(err, sizeof(err), "BD send too frequent, wait %ss", field(s, 4));
    } else {
        snprintf(err, sizeof(err), "BD rejected, FKI reason %s, %s", field(s, 3), field(s, 4));
    }
    finish_send(1, err);
}

/*
 * 处理 TCI（北斗收到的短报文）：解码成 UTF-8 后交给终端任务，由它组 JSON 上报 42214
 *
 * 字段（3.md 表 12）：1 发信方，2 收信方，3 频点，4 时间，5 编码类型，6 数据标识，7 通信数据
 * 解码：
 *   编码类型 3（混编）：数据是 "A4" + GBK 十六进制，bd_mixed_decode() 先还原成 GBK 字节；
 *   编码类型 1（汉字）/ 2（代码），或混编数据格式不对：数据本身当作 GBK / ASCII 文本；
 *   最后统一用 gbk_to_utf8() 转成 UTF-8。
 * 例：$BDTCI,1859836,1859906,2,034122.00,3,0,A43132D2BBB6FE*18 -> 发信方 1859836，正文 "12一二"
 * 上报用的 msg_id / taskid 留空，由终端任务各生成一个标准 UUID。TERM 队列满时这条短报文丢失（打日志）。
 * 日志：先打解析出的字段，混编数据格式不对时说明改按文本处理，最后打解码出的完整 UTF-8 正文。
 *
 * 参数：s —— 拆好的语句；字段少于 7 个时忽略（打日志）
 * 调用：on_line()，经 k_handlers 表分发
 */
static void on_tci(const bd_sentence_t *s)
{
    const char *data;
    int         n = -1;

    if (s->argc < 7) {
        LOG("BD: TCI has only %d fields, ignored", s->argc);
        return;
    }
    data = s->argv[6];
    LOG("BD: TCI parsed: from %s to %s, freq %s, time %s, code type %s, data %u chars",
        s->argv[0], s->argv[1], s->argv[2], s->argv[3], s->argv[4], (unsigned)strlen(data));
    if (atoi(s->argv[4]) == BD_CODE_MIXED) {
        n = bd_mixed_decode(data, s_gbk, sizeof(s_gbk));
        if (n < 0) {
            LOG("BD: TCI data is not valid mixed code (A4 + hex), used as plain text");
        }
    }

    s_out.kind      = TEXT_RECV;
    s_out.dev       = DEV_BD;
    s_out.status    = 0;
    s_out.msg_id[0] = '\0';                 /* 上报用的 msg_id / taskid 由终端任务生成 */
    s_out.taskid[0] = '\0';
    snprintf(s_out.peer, sizeof(s_out.peer), "%s", s->argv[0]);
    if (n >= 0) {
        gbk_to_utf8(s_gbk, (size_t)n, s_out.text, sizeof(s_out.text));
    } else {                                /* 汉字/代码方式：数据本身就是 GBK/ASCII 文本 */
        gbk_to_utf8((const uint8_t *)data, strlen(data), s_out.text, sizeof(s_out.text));
    }
    log_text(s_out.text, strlen(s_out.text), "BD: TCI from %s, text %u bytes UTF-8",
             s_out.peer, (unsigned)strlen(s_out.text));

    if (!term_post(&s_out)) {
        LOG("BD: terminal queue full, message from %s lost", s_out.peer);
    }
}

/*
 * 语句分发表：语句类型（去掉 2 字节发送方标识后的 3 个字母）-> 处理函数。
 * 要处理新的语句，在这里加一行；不在表里的语句（如 SNR、ZTI）直接忽略。
 */
static const struct {
    const char *type;
    void      (*handle)(const bd_sentence_t *s);
} k_handlers[] = {
    { "PWI", on_pwi },
    { "GSS", on_gss },
    { "ICP", on_icp },
    { "FKI", on_fki },
    { "TCI", on_tci },
};

/*
 * 断句器每切出一条完整语句调用一次：打印原文、解析、记下收到时刻，再按语句类型查表分发
 *
 * 原文：CFG_LOG_BD_RAW 为 1 时，解析之前先打印 "BD: rx: $BDFKI,..."（bd_parse() 会改写 line，
 *       所以要先打）；每秒自动输出的 PWI / GSS（k_periodic 表）只在 CFG_LOG_BD_PERIODIC 为 1 时打。
 * bd_parse() 发现格式错误或校验和不对时，打印整条语句和它应有的校验和，然后丢弃；
 * 类型不在 k_handlers 表里的不处理（原文已经打印过了）。
 * 只要校验通过（不管什么类型）就说明串口和北斗设备都在工作，记下时刻供 update_signal() 判断断线，
 * 同时记下串口字节数，update_signal() 据此区分"没有数据"和"有数据但都是乱码"。
 *
 * 参数：line —— 以 '$' 开头、不含换行的一条语句，bd_parse() 会原地改写它
 * 调用：bd_framer_feed()（作为回调传入），运行在 BD 任务里
 */
static void on_line(char *line)
{
    bd_sentence_t s;
    size_t        i;

    if (CFG_LOG_BD_RAW && (CFG_LOG_BD_PERIODIC || !is_periodic(line))) {
        log_text(line, strlen(line), "BD: rx");
    }
    if (!bd_parse(line, &s)) {
        /* 校验和错时 line 没被改动；语句头太短时 '*' 已被改成结束符，bd_checksum() 算出的值一样 */
        log_text(line, strlen(line), "BD: bad sentence (checksum should be *%02X, or bad format), dropped",
                 bd_checksum(line));
        return;
    }
    s_rx_at         = xTaskGetTickCount();
    s_rx_bytes_mark = s_rx_bytes;

    for (i = 0; i < sizeof(k_handlers) / sizeof(k_handlers[0]); i++) {
        if (strcmp(s.type, k_handlers[i].type) == 0) {
            k_handlers[i].handle(&s);
            return;
        }
    }
}

/* --------------------------- 定时事务 --------------------------- */

/*
 * 发读卡指令 $CCICR,0,00*68，北斗应答 ICP，由 on_icp() 处理
 *
 * 调用：bd_task() 主循环；还没读到卡（s_card_ok 为 false）时每 CFG_BD_ICR_RETRY_MS（5s）一次，
 *       上电后第一圈就会发
 */
static void query_card(void)
{
    int len = bd_build(s_tx, sizeof(s_tx), "CCICR,0,00");

    if (len > 0) {
        serial_write(SERIAL_BD, s_tx, (size_t)len, BD_TX_WAIT_MS);
    }
    LOG("BD: query card (ICR)");
    log_tx(s_tx, len);
}

/*
 * 把一路信号折算成 0~4 级：级数 = min(数量, 4)
 *
 * 从没收到过，或最近一次距今超过 CFG_BD_SIGNAL_STALE_MS（5s），视为这一路没有数据，返回 -1，
 * 由 update_signal() 决定怎么组合。tick 相减是无符号运算，计数回绕时也正确。
 *
 * 参数：src —— s_beams 或 s_sats
 *       now —— 当前 tick
 * 返回：0~4 级；-1 表示这一路没有数据
 * 调用：update_signal()
 */
static int sig_level(const sig_src_t *src, TickType_t now)
{
    if (!src->seen || now - src->at > pdMS_TO_TICKS(CFG_BD_SIGNAL_STALE_MS)) {
        return -1;
    }
    return (src->count > SQ_MAX) ? SQ_MAX : src->count;
}

/*
 * 每秒刷新心跳里的 BD.SQ 和 BD.status
 *
 * 串口：超过 CFG_BD_LINK_TIMEOUT_MS（5s）没收到任何有效语句（北斗每秒都会自动输出 PWI，
 *       正常不会这么久没数据）-> 判为异常：status = 2、SQ = 0。s_rx_at 初值是任务启动时刻，
 *       所以上电后先有 5s 等设备开机，这期间按离线处理，之后还是没数据才报异常。
 * SQ  ：由 GSS 可用卫星数和 PWI 新体制波束锁定数共同确定（design.txt），0~4 级
 *       每一路先折算成级数：min(数量, 4)
 *       两路都有数据取平均（四舍五入），只有一路就用这一路，都没有为 0
 * status：串口正常时 SQ 为 0 判离线（1），否则在线（0）
 *
 * 例：波束 3、卫星 6 -> (3 + 4 + 1) / 2 = 4；只有波束 3（区域模块没有 GSS）-> 3；
 *     两路都超过 5s 没更新 -> 0，离线；串口线拔了 -> 异常。
 * 结果加锁写入 dev_status（心跳 42202 的 BD.SQ、BD.status），有变化时打日志。
 * 异常期间如果串口其实一直有字节进来（只是凑不出校验通过的语句），每 5s 提示一次收到了多少字节，
 * 多半是波特率不对、RS422 收发线接反或接错；完全没有字节进来时不提示（线断、设备没电）。
 *
 * 参数：now —— 当前 tick
 * 调用：bd_task() 主循环，每 BD_SQ_PERIOD_MS（1s）一次
 */
static void update_signal(TickType_t now)
{
    bool          link  = (now - s_rx_at <= pdMS_TO_TICKS(CFG_BD_LINK_TIMEOUT_MS));
    int           beams = sig_level(&s_beams, now);
    int           sats  = sig_level(&s_sats, now);
    int           sq;
    uint8_t       status;
    int           old_sq;
    uint8_t       old_status;
    dev_status_t *st;

    if (beams >= 0 && sats >= 0) {
        sq = (beams + sats + 1) / 2;
    } else if (beams >= 0) {
        sq = beams;
    } else if (sats >= 0) {
        sq = sats;
    } else {
        sq = 0;
    }
    if (!link) {
        sq = 0;
    }
    status = !link ? BD_FAULT : (sq > 0) ? BD_ONLINE : BD_OFFLINE;

    st = dev_status_lock();
    old_sq        = st->bd.sq;
    old_status    = st->bd.status;
    st->bd.sq     = (uint8_t)sq;
    st->bd.status = status;
    dev_status_unlock();

    if (old_status != status) {
        LOG("BD: status %u -> %u (%s)", old_status, status,
            (status == BD_FAULT) ? "no valid sentence on serial" : (status == BD_ONLINE) ? "online" : "no signal");
    }
    if (old_sq != sq) {
        LOG("BD: SQ %d -> %d (beams %d, sats %d)", old_sq, sq, beams, sats);
    }
    if (!link && s_rx_bytes != s_rx_bytes_mark &&
        now - s_junk_at >= pdMS_TO_TICKS(CFG_BD_LINK_TIMEOUT_MS)) {
        LOG("BD: %lu bytes received on serial but no valid sentence, check baud rate (%lu) and wiring",
            (unsigned long)(s_rx_bytes - s_rx_bytes_mark), (unsigned long)CFG_BD_BAUDRATE);
        s_rx_bytes_mark = s_rx_bytes;
        s_junk_at       = now;
    }
}

/* ------------------------------ 任务 ------------------------------ */

/*
 * 创建 BD 队列：深度 2，每项一个 text_msg_t（终端任务提交的待发短报文）
 *
 * 北斗发射受卡的服务频度限制（每隔"服务频度"秒才能发一次），排太多也发不出去，
 * 所以只留 2 条，满了让终端立即收到失败。
 * 在 main() 里、调度器启动前调用（TERM 任务一启动就可能调用 bd_submit()）。
 */
void bd_init(void)
{
    s_queue = xQueueCreate(BD_QUEUE_LEN, sizeof(text_msg_t));
    configASSERT(s_queue != NULL);
}

/*
 * 终端任务提交一条待发短报文：放入 BD 队列（按值拷贝，不等待）
 *
 * 参数：req —— kind = TEXT_SEND，peer 是收信方号码，text 是 UTF-8 正文
 * 返回：true 已入队；false 队列满，终端任务随即回 42213 失败
 * 调用：term_task.c 的 forward_text()（经 k_dev 表），运行在 TERM 任务里
 */
bool bd_submit(const text_msg_t *req)
{
    return xQueueSend(s_queue, req, 0) == pdPASS;
}

/*
 * 北斗任务主函数（优先级 4，main() 里直接创建，不用等网络）
 *
 * 启动：向看门狗登记，打开北斗串口（USART6，CFG_BD_BAUDRATE，8N1）。
 * 主循环每一圈（最长约 20ms）：
 *   1. 喂狗；从串口取一段数据（没有就最多等 20ms）交给断句器，切出的每条语句经 on_line() 分发处理；
 *   2. 在途短报文过了 FKI 截止时刻 -> finish_send(失败)；
 *   3. 没有在途短报文且 BD 队列里有请求 -> start_send()（同一时间只发一条）；
 *   4. 还没读到卡且到了重发时刻 -> query_card()，下次在 5s 后；
 *   5. 到了 SQ 刷新时刻 -> update_signal()，下次在 1s 后。
 * 收到的语句只更新数据，所有定时动作都在每圈末尾按时间检查，不依赖收数据的节奏。
 * next_icr / next_sq 初值为 0，所以第一圈就会发 ICR、刷新 SQ。
 *
 * 参数：arg —— 未使用
 */
void bd_task(void *arg)
{
    int        wdg      = wdg_register("BD");
    TickType_t next_icr = 0;
    TickType_t next_sq  = 0;
    bool       opened   = serial_open(SERIAL_BD, CFG_BD_BAUDRATE);

    (void)arg;
    configASSERT(opened);                   /* 只有堆不够建信号量才会失败，停机后由看门狗复位 */
    s_rx_at = xTaskGetTickCount();          /* 断线判断从现在开始计时 */

    for (;;) {
        TickType_t now;
        size_t     n;

        wdg_feed(wdg);
        n = serial_read(SERIAL_BD, s_rx, sizeof(s_rx), BD_POLL_MS);
        if (n > 0U) {
            s_rx_bytes += (uint32_t)n;
            bd_framer_feed(&s_framer, s_rx, n, on_line);
        }

        now = xTaskGetTickCount();
        if (s_pending.active && time_reached(now, s_pending.deadline)) {
            finish_send(1, "BD no response (FKI timeout)");
        }
        if (!s_pending.active && xQueueReceive(s_queue, &s_req, 0) == pdPASS) {
            start_send(&s_req);
        }
        if (!s_card_ok && time_reached(now, next_icr)) {
            query_card();
            next_icr = now + pdMS_TO_TICKS(CFG_BD_ICR_RETRY_MS);
        }
        if (time_reached(now, next_sq)) {
            update_signal(now);
            next_sq = now + pdMS_TO_TICKS(BD_SQ_PERIOD_MS);
        }
    }
}
