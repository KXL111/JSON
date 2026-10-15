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
#define TERM_RX_MAX         6144
#define TERM_TX_MAX         4096

static const struct {
    const char *name;
    uint16_t    result;
    uint16_t    recv;
    bool      (*submit)(const text_msg_t *req);
} k_dev[DEV_COUNT] = {
    [DEV_BD] = { "BD", MSG_BD_MESSAGE_REPLY, MSG_BD_MESSAGE_RECV, bd_submit },
    [DEV_TT] = { "TT", MSG_TT_MESSAGE_REPLY, MSG_TT_MESSAGE_RECV, tt_submit },
};

static udp_link_t    s_link;
static QueueHandle_t s_queue;
static uint64_t      s_time_base_ms;
static text_msg_t    s_msg;
static char          s_rx[TERM_RX_MAX + 1];
static char          s_tx[TERM_TX_MAX];
static unsigned      s_tx_type;

static bool log_wanted(unsigned type)
{
    return CFG_LOG_TERM_HEARTBEAT || (type != MSG_HEARTBEAT_REQUEST && type != MSG_HEARTBEAT_REPLY);
}

static uint64_t uptime_ms(void)
{
    TimeOut_t t;
    uint64_t  ticks;

    vTaskSetTimeOutState(&t);
    ticks = ((uint64_t)(uint32_t)t.xOverflowCount << 32) | (uint64_t)t.xTimeOnEntering;
    return ticks * 1000U / configTICK_RATE_HZ;
}

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

static uint64_t now_unix(void)
{
    uint64_t ms = s_time_base_ms + uptime_ms();

#if CFG_TERM_TS_MS
    return ms;
#else
    return ms / 1000U;
#endif
}

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

static void msg_send(json_writer_t *w)
{
    int  len;
    bool ok;

    json_end_obj(w);
    len = json_end(w);
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

static void reply_status(unsigned type, const char *msg_id, const char *taskid, int status, const char *err)
{
    json_writer_t w;

    msg_begin(&w, msg_id, type);
    json_add_str(&w, "taskid", taskid);
    json_add_int(&w, "status", status);
    json_add_str(&w, KEY_REPLY_ERRMSG, err);
    msg_send(&w);
}

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

    json_add_obj(&w, "taskstate");
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

    json_end_obj(&w);
    msg_send(&w);
}

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

static void handle_json(const char *js, int len)
{
    char        msg_id[APP_ID_LEN];
    char        taskid[APP_ID_LEN];
    char        ts[24];
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

    case MSG_BD_MESSAGE_RECV_ACK:
    case MSG_TT_MESSAGE_RECV_ACK:
    case MSG_TT_CALL_IN_ACK:
    case MSG_TT_CALL_ENDED_ACK:
        value = 0;
        json_get_uint(js, "status", &value);
        LOG("TERM: ack %u for task %s, status %u", type, taskid, (unsigned)value);
        break;

    case MSG_VHF_CHANNEL_SET:
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

void term_init(void)
{
    s_queue = xQueueCreate(TERM_QUEUE_LEN, sizeof(text_msg_t));
    configASSERT(s_queue != NULL);
}

bool term_post(const text_msg_t *msg)
{
    return xQueueSend(s_queue, msg, pdMS_TO_TICKS(TERM_POST_WAIT_MS)) == pdPASS;
}

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
