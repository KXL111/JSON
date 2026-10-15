/*
 * log —— 调试日志，从调试串口输出，每条自动加时间戳(ms)和换行
 *
 * 用法：LOG("BD: card %s, level %d", id, level);
 *       log_text(buf, len, "TERM: rx %d bytes", len);    打印一段原始报文（可以很长，自动分行）
 * app_config.h 里 CFG_LOG_ENABLE 置 0 后不再初始化串口，LOG / log_text 变成空操作。
 */
#ifndef LOG_H
#define LOG_H

#include <stddef.h>

void log_init(void);
void log_printf(const char *fmt, ...);
void log_text(const char *data, size_t len, const char *fmt, ...);

#define LOG(...)    log_printf(__VA_ARGS__)

#endif /* LOG_H */
