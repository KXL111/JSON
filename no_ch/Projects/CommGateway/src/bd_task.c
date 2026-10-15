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

#define BD_POLL_MS          20
#define BD_RX_CHUNK         256
#define BD_TX_WAIT_MS       1500
#define BD_SQ_PERIOD_MS     1000
#define BD_QUEUE_LEN        2
#define BD_GBK_MAX          2000
#define BD_CODE_MIXED       3
#define BD_BEAMS_MAX        16
#define SQ_MAX              4

#if CFG_LOG_BD_PERIODIC
#define LOG_PERIODIC(...)   LOG(__VA_ARGS__)
#else
#define LOG_PERIODIC(...)   ((void)0)
#endif

#define BD_ONLINE           0
#define BD_OFFLINE          1
#define BD_FAULT            2

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

static const uint16_t k_len_cn[5] = {
    CN_MAX_COUNT_LEVEL1, CN_MAX_COUNT_LEVEL2, CN_MAX_COUNT_LEVEL3, CN_MAX_COUNT_LEVEL4, CN_MAX_COUNT_LEVEL5
};
static const uint16_t k_len_en[5] = {
    EN_MAX_COUNT_LEVEL1, EN_MAX_COUNT_LEVEL2, EN_MAX_COUNT_LEVEL3, EN_MAX_COUNT_LEVEL4, EN_MAX_COUNT_LEVEL5
};

typedef struct {
    bool       seen;
    uint8_t    count;
    TickType_t at;
} sig_src_t;

static const char *const k_periodic[] = { "PWI", "GSS" };

static bd_framer_t   s_framer;
static QueueHandle_t s_queue;
static bool          s_card_ok;
static sig_src_t     s_beams;
static sig_src_t     s_sats;
static TickType_t    s_rx_at;
static uint32_t      s_rx_bytes;
static uint32_t      s_rx_bytes_mark;
static TickType_t    s_junk_at;

static struct {
    bool       active;
    TickType_t deadline;
    char       msg_id[APP_ID_LEN];
    char       taskid[APP_ID_LEN];
} s_pending;

static text_msg_t s_req;
static text_msg_t s_out;
static char       s_rx[BD_RX_CHUNK];
static char       s_tx[BD_LINE_MAX];
static uint8_t    s_gbk[BD_GBK_MAX];
static char       s_hex[2 + BD_GBK_MAX * 2 + 1];

static bool time_reached(TickType_t now, TickType_t when)
{
    return (int32_t)(now - when) >= 0;
}

static void set_task_state(const char *taskid, uint8_t progress)
{
    dev_status_t *st = dev_status_lock();

    snprintf(st->task.taskid, sizeof(st->task.taskid), "%s", taskid);
    st->task.progress = progress;
    dev_status_unlock();
}

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

static bool is_periodic(const char *line)
{
    size_t head = strcspn(line, ",*");
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

static void sig_update(sig_src_t *src, int count)
{
    src->count = (uint8_t)((count < 0) ? 0 : (count > 255) ? 255 : count);
    src->at    = xTaskGetTickCount();
    src->seen  = true;
}

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
    idx       = 2 + old_beams * 3;
    if (old_beams < 0 || old_beams > BD_BEAMS_MAX || idx >= s->argc) {
        LOG_PERIODIC("BD: PWI fields not as expected (old beams %d, %d fields), ignored", old_beams, s->argc);
        return;
    }
    new_beams = atoi(s->argv[idx]);
    LOG_PERIODIC("BD: PWI parsed: old beams %d, new beams %d", old_beams, new_beams);
    if (new_beams >= 0 && new_beams <= BD_BEAMS_MAX) {
        sig_update(&s_beams, new_beams);
    }
}

static void on_gss(const bd_sentence_t *s)
{
    if (s->argc >= 1) {
        LOG_PERIODIC("BD: GSS parsed: sats %d", atoi(s->argv[0]));
        sig_update(&s_sats, atoi(s->argv[0]));
    }
}

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
    card.freq_region = (atoi(s->argv[12]) == 1) ? (uint16_t)atoi(s->argv[13]) : 0;
    card.freq_global = (atoi(s->argv[16]) == 1) ? (uint16_t)atoi(s->argv[17]) : 0;
    level = atoi(s->argv[14]);
    if (level >= 1 && level <= 5) {
        card.len_level = (uint8_t)level;
        card.len_cn    = k_len_cn[level - 1];
        card.len_en    = k_len_en[level - 1];
    }

    st = dev_status_lock();
    st->bd.card   = card;
    st->bd.cd     = card.freq_global;
    st->bd.maxlen = card.len_cn;
    dev_status_unlock();

    s_card_ok = true;
    LOG("BD: card %s, freq region %us global %us, level %d (cn %u, en %u)", card.id,
        card.freq_region, card.freq_global, level, card.len_cn, card.len_en);
}

static const char *field(const bd_sentence_t *s, int i)
{
    return (i < s->argc) ? s->argv[i] : "-";
}

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
    s_out.msg_id[0] = '\0';
    s_out.taskid[0] = '\0';
    snprintf(s_out.peer, sizeof(s_out.peer), "%s", s->argv[0]);
    if (n >= 0) {
        gbk_to_utf8(s_gbk, (size_t)n, s_out.text, sizeof(s_out.text));
    } else {
        gbk_to_utf8((const uint8_t *)data, strlen(data), s_out.text, sizeof(s_out.text));
    }
    log_text(s_out.text, strlen(s_out.text), "BD: TCI from %s, text %u bytes UTF-8",
             s_out.peer, (unsigned)strlen(s_out.text));

    if (!term_post(&s_out)) {
        LOG("BD: terminal queue full, message from %s lost", s_out.peer);
    }
}

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

static void on_line(char *line)
{
    bd_sentence_t s;
    size_t        i;

    if (CFG_LOG_BD_RAW && (CFG_LOG_BD_PERIODIC || !is_periodic(line))) {
        log_text(line, strlen(line), "BD: rx");
    }
    if (!bd_parse(line, &s)) {
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

static void query_card(void)
{
    int len = bd_build(s_tx, sizeof(s_tx), "CCICR,0,00");

    if (len > 0) {
        serial_write(SERIAL_BD, s_tx, (size_t)len, BD_TX_WAIT_MS);
    }
    LOG("BD: query card (ICR)");
    log_tx(s_tx, len);
}

static int sig_level(const sig_src_t *src, TickType_t now)
{
    if (!src->seen || now - src->at > pdMS_TO_TICKS(CFG_BD_SIGNAL_STALE_MS)) {
        return -1;
    }
    return (src->count > SQ_MAX) ? SQ_MAX : src->count;
}

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

void bd_init(void)
{
    s_queue = xQueueCreate(BD_QUEUE_LEN, sizeof(text_msg_t));
    configASSERT(s_queue != NULL);
}

bool bd_submit(const text_msg_t *req)
{
    return xQueueSend(s_queue, req, 0) == pdPASS;
}

void bd_task(void *arg)
{
    int        wdg      = wdg_register("BD");
    TickType_t next_icr = 0;
    TickType_t next_sq  = 0;
    bool       opened   = serial_open(SERIAL_BD, CFG_BD_BAUDRATE);

    (void)arg;
    configASSERT(opened);
    s_rx_at = xTaskGetTickCount();

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
