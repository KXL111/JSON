#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bd_proto.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void emit_line(bd_framer_t *f, bd_line_fn on_line)
{
    if (f->len > 1U) {
        f->line[f->len] = '\0';
        on_line(f->line);
    }
    f->len = 0;
}

static bool has_checksum_tail(const bd_framer_t *f)
{
    return f->len >= 4U && f->line[f->len - 3U] == '*' &&
           hexval(f->line[f->len - 2U]) >= 0 && hexval(f->line[f->len - 1U]) >= 0;
}

void bd_framer_feed(bd_framer_t *f, const char *data, size_t n, bd_line_fn on_line)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = data[i];

        if (c == '$') {
            if (has_checksum_tail(f)) {
                emit_line(f, on_line);
            }
            f->len = 0;
        } else if (f->len == 0U) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            emit_line(f, on_line);
            continue;
        }
        if (f->len + 1U >= sizeof(f->line)) {
            f->len = 0;
            continue;
        }
        f->line[f->len++] = c;
    }

    if (has_checksum_tail(f)) {
        emit_line(f, on_line);
    }
}

uint8_t bd_checksum(const char *line)
{
    const char *end = strrchr(line, '*');
    const char *p   = (line[0] == '$') ? line + 1 : line;
    uint8_t     sum = 0;

    if (end == NULL) {
        end = line + strlen(line);
    }
    for (; p < end; p++) {
        sum ^= (uint8_t)*p;
    }
    return sum;
}

bool bd_parse(char *line, bd_sentence_t *s)
{
    char  *star;
    char  *p;
    size_t head;

    if (line[0] != '$') {
        return false;
    }

    star = strrchr(line, '*');
    if (star != NULL) {
        int hi = hexval(star[1]);
        int lo = (hi >= 0) ? hexval(star[2]) : -1;

        if (lo < 0 || bd_checksum(line) != (uint8_t)((hi << 4) | lo)) {
            return false;
        }
        *star = '\0';
    }

    p    = line + 1;
    head = strcspn(p, ",");
    if (head < 4U) {
        return false;
    }
    memcpy(s->type, p + head - 3U, 3);
    s->type[3] = '\0';

    s->argc = 0;
    p += head;
    while (*p == ',' && s->argc < BD_FIELD_MAX) {
        *p++ = '\0';
        s->argv[s->argc++] = p;
        p += strcspn(p, ",");
    }
    return true;
}

int bd_build(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    int     len;
    int     i;
    uint8_t sum = 0;

    if (size < 8U) {
        return -1;
    }
    out[0] = '$';
    va_start(ap, fmt);
    len = vsnprintf(out + 1, size - 1U, fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len + 7U > size) {
        return -1;
    }

    for (i = 1; i <= len; i++) {
        sum ^= (uint8_t)out[i];
    }
    len += 1;
    len += snprintf(out + len, size - (size_t)len, "*%02X\r\n", sum);
    return len;
}

int bd_mixed_encode(const uint8_t *gbk, size_t len, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    size_t n = 0;

    if (2U + len * 2U + 1U > size) {
        return -1;
    }
    out[n++] = 'A';
    out[n++] = '4';
    for (i = 0; i < len; i++) {
        out[n++] = hex[gbk[i] >> 4];
        out[n++] = hex[gbk[i] & 0x0FU];
    }
    out[n] = '\0';
    return (int)n;
}

int bd_mixed_decode(const char *text, uint8_t *out, size_t size)
{
    size_t len = strlen(text);
    size_t i;
    size_t n = 0;

    if (len < 2U || (len % 2U) != 0U || (text[0] != 'A' && text[0] != 'a') || text[1] != '4') {
        return -1;
    }
    for (i = 2; i < len; i += 2U) {
        int hi = hexval(text[i]);
        int lo = hexval(text[i + 1U]);

        if (hi < 0 || lo < 0 || n >= size) {
            return -1;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}
