#include <stdio.h>
#include <string.h>
#include "json.h"

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static const char *string_end(const char *p)
{
    for (; *p != '\0'; p++) {
        if (*p == '\\') {
            if (*++p == '\0') {
                return NULL;
            }
        } else if (*p == '"') {
            return p;
        }
    }
    return NULL;
}

static const char *find_value(const char *js, const char *key)
{
    size_t      klen = strlen(key);
    const char *p    = js;

    while ((p = strchr(p, '"')) != NULL) {
        const char *s = p + 1;
        const char *e = string_end(s);

        if (e == NULL) {
            return NULL;
        }
        p = skip_ws(e + 1);
        if (*p == ':' && (size_t)(e - s) == klen && memcmp(s, key, klen) == 0) {
            return skip_ws(p + 1);
        }
    }
    return NULL;
}

static long hex4(const char *p)
{
    long v = 0;
    int  i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

static size_t utf8_put(unsigned long cp, char *out)
{
    if (cp < 0x80UL) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        out[0] = (char)(0xC0UL | (cp >> 6));
        out[1] = (char)(0x80UL | (cp & 0x3FUL));
        return 2;
    }
    if (cp < 0x10000UL) {
        out[0] = (char)(0xE0UL | (cp >> 12));
        out[1] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
        out[2] = (char)(0x80UL | (cp & 0x3FUL));
        return 3;
    }
    out[0] = (char)(0xF0UL | (cp >> 18));
    out[1] = (char)(0x80UL | ((cp >> 12) & 0x3FUL));
    out[2] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
    out[3] = (char)(0x80UL | (cp & 0x3FUL));
    return 4;
}

static long parse_u_escape(const char **pp)
{
    const char *p  = *pp;
    long        cp = hex4(p + 1);

    if (cp < 0) {
        return -1;
    }
    p += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
        long lo = hex4(p + 3);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000L + ((cp - 0xD800L) << 10) + (lo - 0xDC00L);
            p += 6;
        }
    }
    *pp = p;
    return cp;
}

bool json_get_str(const char *js, const char *key, char *out, size_t size)
{
    const char *p = find_value(js, key);
    size_t      n = 0;

    if (p == NULL || size == 0) {
        return false;
    }

    if (*p != '"') {
        while (*p != '\0' && strchr(",}] \t\r\n", *p) == NULL) {
            if (n + 1 >= size) {
                return false;
            }
            out[n++] = *p++;
        }
        out[n] = '\0';
        return n > 0;
    }

    for (p++; *p != '"'; p++) {
        char   ch[4];
        size_t len = 1;

        if (*p == '\0') {
            return false;
        }
        ch[0] = *p;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': ch[0] = '\n'; break;
            case 'r': ch[0] = '\r'; break;
            case 't': ch[0] = '\t'; break;
            case 'b': ch[0] = '\b'; break;
            case 'f': ch[0] = '\f'; break;
            case 'u': {
                long cp = parse_u_escape(&p);
                if (cp < 0) {
                    return false;
                }
                len = utf8_put((unsigned long)cp, ch);
                break;
            }
            case '\0': return false;
            default:   ch[0] = *p; break;
            }
        }
        if (n + len >= size) {
            return false;
        }
        memcpy(out + n, ch, len);
        n += len;
    }
    out[n] = '\0';
    return true;
}

bool json_get_uint(const char *js, const char *key, uint64_t *out)
{
    const char *p      = find_value(js, key);
    uint64_t    v      = 0;
    int         digits = 0;

    if (p == NULL) {
        return false;
    }
    if (*p == '"') {
        p = skip_ws(p + 1);
    }
    while (*p >= '0' && *p <= '9' && digits < 19) {
        v = v * 10U + (uint64_t)(*p - '0');
        p++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = v;
    return true;
}

const char *json_check(const char *js, size_t len)
{
    size_t i;
    int    depth  = 0;
    bool   in_str = false;

    if (memchr(js, '\0', len) != NULL) {
        return "contains 0x00, data after it is ignored";
    }
    i = (size_t)(skip_ws(js) - js);
    if (i >= len || js[i] != '{') {
        return "does not start with '{'";
    }
    for (; i < len; i++) {
        char c = js[i];

        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if ((c == '}' || c == ']') && --depth == 0) {
            break;
        }
    }
    if (in_str) {
        return "string not closed (truncated?)";
    }
    if (depth != 0) {
        return "braces not closed (truncated?)";
    }
    if (*skip_ws(js + i + 1) != '\0') {
        return "extra data after the JSON object";
    }
    return NULL;
}

static void put(json_writer_t *w, const char *s, size_t n)
{
    if (w->len + n >= w->size) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void put_string(json_writer_t *w, const char *s)
{
    const char *run = s;

    put(w, "\"", 1);
    for (;; s++) {
        unsigned char c = (unsigned char)*s;
        char          esc[8];

        if (c >= 0x20 && c != '"' && c != '\\') {
            continue;
        }
        put(w, run, (size_t)(s - run));
        if (c == '\0') {
            break;
        }
        switch (c) {
        case '"':  put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2);  break;
        case '\r': put(w, "\\r", 2);  break;
        case '\t': put(w, "\\t", 2);  break;
        default:
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            put(w, esc, 6);
            break;
        }
        run = s + 1;
    }
    put(w, "\"", 1);
}

static void put_key(json_writer_t *w, const char *key)
{
    if (w->need_comma[w->depth]) {
        put(w, ",", 1);
    }
    w->need_comma[w->depth] = true;
    put_string(w, key);
    put(w, ":", 1);
}

void json_begin(json_writer_t *w, char *buf, size_t size)
{
    w->buf           = buf;
    w->size          = size;
    w->len           = 0;
    w->depth         = 0;
    w->need_comma[0] = false;
    w->overflow      = false;
    put(w, "{", 1);
}

void json_add_str(json_writer_t *w, const char *key, const char *val)
{
    put_key(w, key);
    put_string(w, val);
}

void json_add_int(json_writer_t *w, const char *key, long val)
{
    char num[24];
    int  n = snprintf(num, sizeof(num), "%ld", val);

    put_key(w, key);
    put(w, num, (size_t)n);
}

void json_add_u64(json_writer_t *w, const char *key, uint64_t val)
{
    char   num[21];
    size_t pos = sizeof(num);

    do {
        num[--pos] = (char)('0' + (int)(val % 10U));
        val /= 10U;
    } while (val != 0U);

    put_key(w, key);
    put(w, num + pos, sizeof(num) - pos);
}

void json_add_obj(json_writer_t *w, const char *key)
{
    put_key(w, key);
    put(w, "{", 1);
    if (w->depth + 1 >= JSON_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    w->depth++;
    w->need_comma[w->depth] = false;
}

void json_end_obj(json_writer_t *w)
{
    put(w, "}", 1);
    if (w->depth > 0) {
        w->depth--;
    }
}

int json_end(json_writer_t *w)
{
    put(w, "}", 1);
    return w->overflow ? -1 : (int)w->len;
}
