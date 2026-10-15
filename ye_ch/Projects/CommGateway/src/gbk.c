/*
 * gbk.c —— UTF-8 <-> GBK 转换（查表）
 *
 * 对照表在 gbk_table.c（由 tools/gen_gbk_table.ps1 从 Windows 代码页 936 生成）：
 *   GBK -> Unicode：gbk_table_to_uni[]，按 (首字节 - 0x81) × 191 + (尾字节 - 0x40) 直接下标访问；
 *   Unicode -> GBK：gbk_table_uni[] 按 Unicode 升序，gbk_table_gbk[] 与之一一对应，二分查找。
 * ASCII（0x00~0x7F）两种编码相同，不查表。
 *
 * 调用关系（纯函数，都在 BD 任务里被调用）：
 *   gbk_from_utf8() -> utf8_get() + uni_to_gbk()            北斗发送：终端正文 -> GBK
 *   gbk_to_utf8()   -> 查 gbk_table_to_uni[] + utf8_put()   北斗接收：GBK -> 终端正文
 */
#include <string.h>
#include "gbk.h"

/* GBK 双字节码：首字节 0x81~0xFE，尾字节 0x40~0xFE（共 191 个） */
#define LEAD_MIN    0x81U
#define LEAD_MAX    0xFEU
#define TRAIL_MIN   0x40U
#define TRAIL_MAX   0xFEU
#define TRAIL_SPAN  (TRAIL_MAX - TRAIL_MIN + 1U)

/* 对照表，见 gbk_table.c */
extern const uint16_t gbk_table_to_uni[];
extern const uint16_t gbk_table_count;
extern const uint16_t gbk_table_uni[];
extern const uint16_t gbk_table_gbk[];

/*
 * 解出一个 UTF-8 字符，返回占用的字节数；非法序列按 1 字节、码点 '?' 处理
 *
 * 1~3 字节的字符解出真实码点；4 字节的字符（基本平面以外，如 emoji）GBK 里肯定没有，
 * 统一返回 0x10000，由 uni_to_gbk() 查不到后换成 '?'。
 * 每个后续字节都要检查 10xxxxxx 格式，遇到结束符 '\0' 也会判为非法，所以不会读过字符串末尾。
 *
 * 参数：s  —— 指向一个字符的首字节
 *       cp —— 输出码点
 * 返回：这个字符占的字节数 1~4
 * 调用：gbk_from_utf8()
 */
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
        *cp = 0x10000U;                     /* 基本平面以外的字符（如 emoji），GBK 里没有 */
        return 4;
    }
    *cp = '?';
    return 1;
}

/*
 * Unicode 码点 -> GBK 双字节码，在升序表 gbk_table_uni[] 里二分查找
 *
 * 表约 2.2 万项，最多比较 15 次。
 *
 * 参数：cp —— Unicode 码点（>= 0x80）
 * 返回：GBK 码（高字节在高 8 位，如 "一" 返回 0xD2BB）；GBK 里没有这个字返回 0
 * 调用：gbk_from_utf8()
 */
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

/*
 * UTF-8 -> GBK（北斗发送用）
 *
 * 逐个解出 UTF-8 字符：ASCII 原样写 1 字节；其他字符查表得到 GBK 码，高字节在前写 2 字节；
 * GBK 里没有的字符（emoji、部分生僻字）和非法 UTF-8 序列都换成 '?'，不会中断转换。
 * 例："12一二" -> 31 32 D2 BB B6 FE，返回 6
 *
 * 参数：utf8       —— 以 '\0' 结尾的 UTF-8 文本
 *       out / size —— 输出缓冲及大小（不写结束符）
 * 返回：写入的字节数；out 放不下返回 -1（北斗任务据此回 "msg too long"）
 * 调用：bd_task.c 的 start_send()
 */
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
        if (code < 0x100U) {                /* 单字节：ASCII 或替换用的 '?' */
            if (n + 1U > size) {
                return -1;
            }
            out[n++] = (uint8_t)code;
        } else {                            /* 双字节汉字：高字节在前 */
            if (n + 2U > size) {
                return -1;
            }
            out[n++] = (uint8_t)(code >> 8);
            out[n++] = (uint8_t)code;
        }
    }
    return (int)n;
}

/*
 * 码点写成 UTF-8，返回字节数（这里只会出现基本平面字符）
 *
 * GBK 表里的字符都在基本平面（< 0x10000），所以最多 3 字节，比 json.c 里的版本少一个分支。
 *
 * 参数：cp  —— 码点
 *       out —— 至少 3 字节的输出缓冲（不写结束符）
 * 调用：gbk_to_utf8()
 */
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

/*
 * GBK -> UTF-8（北斗接收用）
 *
 * 逐字节判断：
 *   < 0x80：ASCII，1 字节；
 *   首字节 0x81~0xFE 且尾字节 0x40~0xFE：双字节汉字，查 gbk_table_to_uni[]（表里的空位换成 '?'）；
 *   其他（孤立的高字节、末尾只剩半个汉字）：换成 '?'，跳过 1 字节。
 * 输出放不下下一个字符时就停止，截断在完整字符边界上，不会出现半个 UTF-8 字符。
 * 例：31 32 D2 BB B6 FE -> "12一二"
 *
 * 参数：gbk / len  —— GBK 字节及长度（不要求 '\0' 结尾）
 *       out / size —— 输出缓冲及大小，结果始终以 '\0' 结尾
 * 返回：写入的字节数（不含结束符）
 * 调用：bd_task.c 的 on_tci()
 */
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
        if (n + chlen >= size) {            /* 要给结束符留 1 字节 */
            break;
        }
        memcpy(out + n, ch, chlen);
        n += chlen;
    }
    out[n] = '\0';
    return n;
}
