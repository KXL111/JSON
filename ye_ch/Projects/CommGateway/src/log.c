/*
 * log.c —— 调试日志：USART1（PA9 发送 / PA10 接收），阻塞发送
 *
 * 探索者板上 USART1 经 CH340 接到 USB_232 口，电脑上装好 CH340 驱动就能用串口助手看日志。
 *
 * 调度器启动后用互斥锁保证多任务打印不交错；不要在中断里调用。
 */
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
#define LOG_LINE_MAX    256     /* 一行日志最长字节数（含时间戳和 \r\n） */
#define LOG_TEXT_WIDTH  200     /* log_text() 每行大约在这个位置换行 */

static SemaphoreHandle_t s_lock;            /* 多任务打印互斥 */
static char              s_line[LOG_LINE_MAX];  /* 一行日志的格式化缓冲，持锁时使用 */

/*
 * 初始化调试串口：USART1，PA9 发送 / PA10 接收（AF7），波特率 CFG_LOG_BAUDRATE，8N1，只开发送
 *
 * 同时创建日志互斥锁（调度器启动前创建是允许的，启动后才会用到）。
 * 调用：main()，最先调用，后面的初始化出问题也能看到日志
 */
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
    USART_StructInit(&u);                                   /* 8 位数据、1 位停止、无校验、无流控 */
    u.USART_BaudRate = CFG_LOG_BAUDRATE;
    u.USART_Mode     = USART_Mode_Tx;
    USART_Init(LOG_USART, &u);
    USART_Cmd(LOG_USART, ENABLE);

    s_lock = xSemaphoreCreateMutex();
}

/*
 * 阻塞发送 len 个字节：每个字节都等发送数据寄存器空（TXE）再写数据寄存器
 *
 * 115200 波特率下每字节约 87us，一条 100 字节的日志约 9ms，期间 CPU 忙等，
 * 所以主循环里不要频繁打日志。
 * 调用：log_printf()
 */
static void uart_write(const char *s, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        while (RESET == USART_GetFlagStatus(LOG_USART, USART_FLAG_TXE)) {
        }
        USART_SendData(LOG_USART, (uint8_t)s[i]);
    }
}

/*
 * 拿日志互斥锁：调度器运行后才加锁；调度器启动前（main() 里）只有一个执行流，不需要锁
 *
 * 返回：true 加了锁，打印完要用 log_unlock(true) 释放
 * 调用：log_printf()、log_text()
 */
static bool log_lock(void)
{
    bool locked = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    if (locked) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    return locked;
}

/*
 * 释放日志互斥锁，参数是 log_lock() 的返回值
 * 调用：log_printf()、log_text()
 */
static void log_unlock(bool locked)
{
    if (locked) {
        xSemaphoreGive(s_lock);
    }
}

/*
 * 在行缓冲 s_line 开头写入时间戳 "[tick] "，tick 即开机以来的毫秒数
 *
 * 返回：写入的长度
 * 调用：log_printf()、log_text()，持锁时
 */
static int line_head(void)
{
    return snprintf(s_line, sizeof(s_line), "[%lu] ", (unsigned long)xTaskGetTickCount());
}

/*
 * 行缓冲 s_line 里已有 len 个字符，补上 \r\n 后从调试串口发出
 *
 * len 是 snprintf 累加出来的"本该写入的长度"，内容太长被截断时会超过缓冲，
 * 这里统一截到 LOG_LINE_MAX - 3，保证 \r\n 放得下。
 * 调用：log_printf()、log_text()，持锁时
 */
static void line_out(int len)
{
    if (len > LOG_LINE_MAX - 3) {
        len = LOG_LINE_MAX - 3;
    }
    s_line[len++] = '\r';
    s_line[len++] = '\n';
    uart_write(s_line, len);
}

/*
 * 格式化输出一条日志："[tick] 内容\r\n"，tick 即开机以来的毫秒数
 *
 * 调度器运行后先拿互斥锁，保证多个任务的日志不交错（共用行缓冲 s_line）；
 * 调度器启动前（main() 里）不加锁。超过 LOG_LINE_MAX（256 字节）的内容截断，
 * 要完整打印一段长报文用 log_text()。
 * 不能在中断里调用（要拿互斥锁、要忙等串口）。
 *
 * 参数：fmt, ... —— printf 风格的格式串和参数，一般通过 LOG() 宏调用
 * 调用：所有模块
 */
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

/*
 * 打印一段原始数据（收发的报文），先写说明文字，数据跟在后面，太长自动分成多行
 *
 * 输出格式：
 *   [tick] 说明: 数据的开头……
 *       数据的后续……                  （续行以 4 个空格开头，不带时间戳）
 *       ... N more bytes not shown      （超过 CFG_LOG_RAW_MAX 的部分不打印）
 * 显示规则：
 *   - 可显示的 ASCII 原样输出；
 *   - 控制字符（0x00~0x1F、0x7F，包括回车换行）写成 \xHH，如报文末尾的回车换行显示为 \x0D\x0A。
 *     不写成 \r \n，是为了和 JSON 正文里本来就有的 "\n" 转义区分开；
 *   - 0x80 以上的字节原样输出：终端 JSON 里的中文是 UTF-8，串口助手选 UTF-8 编码就能正常显示
 *     （北斗汉字方式的 TCI 原始数据是 GBK，要看它得选 GBK）；
 *   - 每行写到约 LOG_TEXT_WIDTH 个字符处换行，换行不会落在一个 UTF-8 汉字的中间。
 * 整段打完才释放互斥锁，其他任务的日志不会插在中间。
 * 115200 波特率下每 1KB 约 90ms，期间调用者忙等，所以打印量由 CFG_LOG_RAW_MAX 封顶。
 *
 * 参数：data / len —— 要显示的数据（不要求以 '\0' 结尾，中间有 0x00 也照样显示）
 *       fmt, ...   —— printf 风格的说明文字，如 "TERM: rx %d bytes"
 * 调用：term_task.c（终端收发的 JSON、解析出的正文）、bd_task.c（北斗收发的语句、TCI 正文）
 */
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
    if (n > LOG_TEXT_WIDTH) {               /* 说明文字太长就截断，给数据留出位置 */
        n = LOG_TEXT_WIDTH;
    }
    n += snprintf(s_line + n, sizeof(s_line) - (size_t)n, "%s", (len == 0U) ? ": (empty)" : ": ");

    for (i = 0; i < shown; i++) {
        unsigned char c = (unsigned char)data[i];

        /* 写够一行、且这个字节不是 UTF-8 的后续字节（10xxxxxx）时换行；
         * 行缓冲快满时不管是什么字节都换行（要留出一个 \xHH 和 \r\n 的位置） */
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

#else   /* 日志关闭：保留空函数，调用处不用改 */

/*
 * 日志关闭时的空实现：不初始化串口，不输出任何内容。
 * 注意 LOG() 的参数仍然会被求值，例如 main() 里的 reset_reason() 照样执行、照样清复位标志。
 */
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

#endif /* CFG_LOG_ENABLE */
