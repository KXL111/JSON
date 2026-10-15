#include <string.h>
#include "gbk.h"

#define LEAD_MIN    0x81U
#define LEAD_MAX    0xFEU
#define TRAIL_MIN   0x40U
#define TRAIL_MAX   0xFEU
#define TRAIL_SPAN  (TRAIL_MAX - TRAIL_MIN + 1U)

extern const uint16_t gbk_table_to_uni[];
extern const uint16_t gbk_table_count;
extern const uint16_t gbk_table_uni[];
extern const uint16_t gbk_table_gbk[];

static size_t utf8_get(const uint8_t *s, uint32_t *cp)
{
    if (s[0] < 0x80U) {
        *cp = s[0];
        return 1;
    }
    if ((s[0] & 0xE0U) == 0xC0U && (s[1] & 0xC0U) == 0x80U) {
        *cp = ((uint32_t)(s[0] & 0x1FU) << 6) | (s[1] & 0x3FU);
        return 2;
    }
    if ((s[0] & 0xF0U) == 0xE0U && (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U) {
        *cp = ((uint32_t)(s[0] & 0x0FU) << 12) | ((uint32_t)(s[1] & 0x3FU) << 6) | (s[2] & 0x3FU);
        return 3;
    }
    if ((s[0] & 0xF8U) == 0xF0U && (s[1] & 0xC0U) == 0x80U &&
        (s[2] & 0xC0U) == 0x80U && (s[3] & 0xC0U) == 0x80U) {
        *cp = 0x10000U;
        return 4;
    }
    *cp = '?';
    return 1;
}

static uint16_t uni_to_gbk(uint32_t cp)
{
    int lo = 0;
    int hi = (int)gbk_table_count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (gbk_table_uni[mid] == cp) {
            return gbk_table_gbk[mid];
        }
        if (gbk_table_uni[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

int gbk_from_utf8(const char *utf8, uint8_t *out, size_t size)
{
    const uint8_t *s = (const uint8_t *)utf8;
    size_t         n = 0;

    while (*s != 0U) {
        uint32_t cp;
        uint16_t code;

        s += utf8_get(s, &cp);
        code = (cp < 0x80U) ? (uint16_t)cp : uni_to_gbk(cp);
        if (code == 0U) {
            code = '?';
        }
        if (code < 0x100U) {
            if (n + 1U > size) {
                return -1;
            }
            out[n++] = (uint8_t)code;
        } else {
            if (n + 2U > size) {
                return -1;
            }
            out[n++] = (uint8_t)(code >> 8);
            out[n++] = (uint8_t)code;
        }
    }
    return (int)n;
}

static size_t utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80U) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800U) {
        out[0] = (char)(0xC0U | (cp >> 6));
        out[1] = (char)(0x80U | (cp & 0x3FU));
        return 2;
    }
    out[0] = (char)(0xE0U | (cp >> 12));
    out[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    out[2] = (char)(0x80U | (cp & 0x3FU));
    return 3;
}

size_t gbk_to_utf8(const uint8_t *gbk, size_t len, char *out, size_t size)
{
    size_t i = 0;
    size_t n = 0;

    if (size == 0U) {
        return 0;
    }
    while (i < len) {
        uint32_t cp;
        char     ch[3];
        size_t   chlen;

        if (gbk[i] < 0x80U) {
            cp = gbk[i++];
        } else if (i + 1U < len && gbk[i] >= LEAD_MIN && gbk[i] <= LEAD_MAX &&
                   gbk[i + 1U] >= TRAIL_MIN && gbk[i + 1U] <= TRAIL_MAX) {
            cp = gbk_table_to_uni[(gbk[i] - LEAD_MIN) * TRAIL_SPAN + (gbk[i + 1U] - TRAIL_MIN)];
            if (cp == 0U) {
                cp = '?';
            }
            i += 2U;
        } else {
            cp = '?';
            i++;
        }

        chlen = utf8_put(cp, ch);
        if (n + chlen >= size) {
            break;
        }
        memcpy(out + n, ch, chlen);
        n += chlen;
    }
    out[n] = '\0';
    return n;
}
