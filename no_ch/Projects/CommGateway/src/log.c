#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "app_config.h"
#include "log.h"

#if CFG_LOG_ENABLE

#define LOG_USART       USART1
#define LOG_GPIO        GPIOA
#define LOG_TX_PIN      GPIO_Pin_9
#define LOG_RX_PIN      GPIO_Pin_10
#define LOG_LINE_MAX    256
#define LOG_TEXT_WIDTH  200

static SemaphoreHandle_t s_lock;
static char              s_line[LOG_LINE_MAX];

void log_init(void)
{
    GPIO_InitTypeDef  g;
    USART_InitTypeDef u;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    GPIO_PinAFConfig(LOG_GPIO, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(LOG_GPIO, GPIO_PinSource10, GPIO_AF_USART1);
    g.GPIO_Pin   = LOG_TX_PIN | LOG_RX_PIN;
    g.GPIO_Mode  = GPIO_Mode_AF;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(LOG_GPIO, &g);

    USART_DeInit(LOG_USART);
    USART_StructInit(&u);
    u.USART_BaudRate = CFG_LOG_BAUDRATE;
    u.USART_Mode     = USART_Mode_Tx;
    USART_Init(LOG_USART, &u);
    USART_Cmd(LOG_USART, ENABLE);

    s_lock = xSemaphoreCreateMutex();
}

static void uart_write(const char *s, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        while (RESET == USART_GetFlagStatus(LOG_USART, USART_FLAG_TXE)) {
        }
        USART_SendData(LOG_USART, (uint8_t)s[i]);
    }
}

static bool log_lock(void)
{
    bool locked = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    if (locked) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    return locked;
}

static void log_unlock(bool locked)
{
    if (locked) {
        xSemaphoreGive(s_lock);
    }
}

static int line_head(void)
{
    return snprintf(s_line, sizeof(s_line), "[%lu] ", (unsigned long)xTaskGetTickCount());
}

static void line_out(int len)
{
    if (len > LOG_LINE_MAX - 3) {
        len = LOG_LINE_MAX - 3;
    }
    s_line[len++] = '\r';
    s_line[len++] = '\n';
    uart_write(s_line, len);
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    int     len;
    bool    locked = log_lock();

    len = line_head();
    va_start(ap, fmt);
    len += vsnprintf(s_line + len, sizeof(s_line) - (size_t)len, fmt, ap);
    va_end(ap);
    line_out(len);

    log_unlock(locked);
}

void log_text(const char *data, size_t len, const char *fmt, ...)
{
    va_list ap;
    size_t  shown = (len > (size_t)CFG_LOG_RAW_MAX) ? (size_t)CFG_LOG_RAW_MAX : len;
    size_t  i;
    int     n;
    bool    locked = log_lock();

    n = line_head();
    va_start(ap, fmt);
    n += vsnprintf(s_line + n, sizeof(s_line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > LOG_TEXT_WIDTH) {
        n = LOG_TEXT_WIDTH;
    }
    n += snprintf(s_line + n, sizeof(s_line) - (size_t)n, "%s", (len == 0U) ? ": (empty)" : ": ");

    for (i = 0; i < shown; i++) {
        unsigned char c = (unsigned char)data[i];

        if ((n >= LOG_TEXT_WIDTH && (c & 0xC0U) != 0x80U) || n >= LOG_LINE_MAX - 7) {
            line_out(n);
            n = snprintf(s_line, sizeof(s_line), "    ");
        }
        if (c < 0x20U || c == 0x7FU) {
            n += snprintf(s_line + n, sizeof(s_line) - (size_t)n, "\\x%02X", c);
        } else {
            s_line[n++] = (char)c;
        }
    }
    line_out(n);

    if (shown < len) {
        n = snprintf(s_line, sizeof(s_line), "    ... %u more bytes not shown (CFG_LOG_RAW_MAX = %u)",
                     (unsigned)(len - shown), (unsigned)CFG_LOG_RAW_MAX);
        line_out(n);
    }

    log_unlock(locked);
}

#else

void log_init(void)
{
}

void log_printf(const char *fmt, ...)
{
    (void)fmt;
}

void log_text(const char *data, size_t len, const char *fmt, ...)
{
    (void)data;
    (void)len;
    (void)fmt;
}

#endif
