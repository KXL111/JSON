#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "app_config.h"
#include "log.h"
#include "serial.h"
#include "tt_task.h"
#include "wdg.h"

#define TT_POLL_MS      100
#define TT_LINE_MAX     256
#define TT_LOG_MAX      120

static char   s_rx[128];
static char   s_line[TT_LINE_MAX];
static size_t s_len;

bool tt_submit(const text_msg_t *req)
{
    (void)req;
    return false;
}

static void on_line(char *line)
{
    size_t len = strlen(line);
    size_t i;

    if (len > TT_LOG_MAX) {
        line[TT_LOG_MAX] = '\0';
    }
    for (i = 0; line[i] != '\0'; i++) {
        if ((unsigned char)line[i] < 0x20U || (unsigned char)line[i] > 0x7EU) {
            line[i] = '.';
        }
    }
    LOG("TT: rx \"%s\"%s", line, (len > TT_LOG_MAX) ? "..." : "");
}

static void feed(const char *data, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = data[i];

        if (c == '\r' || c == '\n') {
            if (s_len > 0U) {
                s_line[s_len] = '\0';
                on_line(s_line);
                s_len = 0;
            }
        } else if (s_len + 1U < sizeof(s_line)) {
            s_line[s_len++] = c;
        }
    }
}

void tt_task(void *arg)
{
    int  wdg    = wdg_register("TT");
    bool opened = serial_open(SERIAL_TT, CFG_TT_BAUDRATE);

    (void)arg;
    configASSERT(opened);

    for (;;) {
        size_t n;

        wdg_feed(wdg);
        n = serial_read(SERIAL_TT, s_rx, sizeof(s_rx), TT_POLL_MS);
        if (n > 0U) {
            feed(s_rx, n);
        }
    }
}
