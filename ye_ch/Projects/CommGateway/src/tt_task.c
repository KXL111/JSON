/*
 * tt_task.c —— 天通任务（占位）：打开 RS232 串口、喂狗，收到的数据按行打日志
 *
 * 天通模块一般是按行输出的文本（AT 指令风格，"\r\n" 结尾），先按行切分打印出来，
 * 联调时从调试串口就能看到天通实际输出了什么，再据此补协议解析（on_line()）。
 *
 * 函数调用关系：
 *   tt_task() -> serial_read() -> feed() -> on_line()     每收到一整行打一条日志
 *   tt_submit()：由 TERM 任务调用，目前固定返回 false
 */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "app_config.h"
#include "log.h"
#include "serial.h"
#include "tt_task.h"
#include "wdg.h"

#define TT_POLL_MS      100     /* 串口没数据时最多等这么久，即主循环最长周期 */
#define TT_LINE_MAX     256     /* 一行最长字节数，超出部分截断 */
#define TT_LOG_MAX      120     /* 日志里最多显示一行的前这么多个字符 */

static char   s_rx[128];                    /* 从串口取出的一段数据 */
static char   s_line[TT_LINE_MAX];          /* 正在拼的一行 */
static size_t s_len;                        /* s_line 里已有的字节数 */

/*
 * 终端任务提交一条天通短信（占位实现）
 *
 * 天通协议还没定，固定返回 false，终端任务随即回 42209、status = 1、
 * "device busy or not available"。
 * 接入时改为：入队（参照 bd_submit()），由 tt_task 发送，结果和收到的短信用 term_post()
 * 交回终端任务（dev = DEV_TT），终端任务会自动回 42209 / 上报 42210。
 *
 * 参数：req —— kind = TEXT_SEND 的请求（未使用）
 * 返回：false
 * 调用：term_task.c 的 forward_text()（经 k_dev 表），运行在 TERM 任务里
 */
bool tt_submit(const text_msg_t *req)
{
    (void)req;
    return false;
}

/*
 * 处理天通输出的一整行（不含换行）：目前只打日志
 *
 * 不可显示的字节换成 '.'，免得把调试串口的终端软件搞乱；太长的只显示前 TT_LOG_MAX 个字符。
 * 天通协议到位后，在这里按行内容分发（信号质量、入网状态、收到短信、来电等）。
 *
 * 参数：line —— 以 '\0' 结尾的一行（会被原地改写）
 * 调用：feed()
 */
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

/*
 * 把串口取到的一段数据拼成行：遇到 '\r' 或 '\n' 交出一行，空行跳过，超长部分截断
 *
 * 参数：data / n —— 本次从串口取出的数据
 * 调用：tt_task() 主循环
 */
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

/*
 * 天通任务主函数（优先级 3，main() 里直接创建，不用等网络）
 *
 * 结构和北斗任务一样：登记看门狗 -> 打开天通串口（USART3，CFG_TT_BAUDRATE，8N1）
 * -> 主循环：喂狗、取串口数据（没有就最多等 100ms）、按行打日志。
 *
 * 参数：arg —— 未使用
 */
void tt_task(void *arg)
{
    int  wdg    = wdg_register("TT");
    bool opened = serial_open(SERIAL_TT, CFG_TT_BAUDRATE);

    (void)arg;
    configASSERT(opened);                   /* 只有堆不够建信号量才会失败，停机后由看门狗复位 */

    for (;;) {
        size_t n;

        wdg_feed(wdg);
        n = serial_read(SERIAL_TT, s_rx, sizeof(s_rx), TT_POLL_MS);
        if (n > 0U) {
            feed(s_rx, n);
        }
    }
}
