#ifndef LOG_H
#define LOG_H

#include <stddef.h>

void log_init(void);
void log_printf(const char *fmt, ...);
void log_text(const char *data, size_t len, const char *fmt, ...);

#define LOG(...)    log_printf(__VA_ARGS__)

#endif
