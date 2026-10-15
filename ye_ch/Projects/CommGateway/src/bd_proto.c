/*
 * bd_proto.c —— 北斗语句编解码
 *
 * 纯函数模块：不碰串口、任务和全局状态，所有状态都由调用方传入，可以单独在 PC 上测试。
 *   断句：bd_framer_feed() -> has_checksum_tail() / emit_line() -> 回调 on_line（bd_task.c）
 *   解析：bd_parse()       校验和（bd_checksum()）、语句类型、拆字段
 *   组包：bd_build()       自动加 '$'、校验和、换行
 *   混编：bd_mixed_encode() / bd_mixed_decode()   GBK <-> "A4" + 十六进制
 *   hexval() 是内部工具，被断句、解析、混编解码共用。
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bd_proto.h"

/*
 * 一个十六进制字符转成数值，大小写都认
 *
 * 返回：0~15；不是十六进制字符返回 -1
 * 调用：has_checksum_tail()（判断结尾 "*hh"）、bd_parse()（校验和）、bd_mixed_decode()
 */
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

/* ------------------------------ 断句 ------------------------------ */

/*
 * 把断句器里攒好的一条语句交出去，然后清空缓冲
 *
 * 长度不超过 1（只有一个 '$'）的不算语句，直接丢弃。交出前补上结束符，
 * 回调可以把它当普通 C 字符串处理，也可以原地改写（bd_parse() 就会改写）。
 *
 * 参数：f       —— 断句器
 *       on_line —— 每条语句的处理回调
 * 调用：bd_framer_feed()
 */
static void emit_line(bd_framer_t *f, bd_line_fn on_line)
{
    if (f->len > 1U) {
        f->line[f->len] = '\0';
        on_line(f->line);
    }
    f->len = 0;
}

/*
 * 缓冲里攒的内容是否已经以 "*hh" 结尾，即一条带校验和的完整语句、只是还没收到换行
 *
 * 调用：bd_framer_feed()
 */
static bool has_checksum_tail(const bd_framer_t *f)
{
    return f->len >= 4U && f->line[f->len - 3U] == '*' &&
           hexval(f->line[f->len - 2U]) >= 0 && hexval(f->line[f->len - 1U]) >= 0;
}

/*
 * 断句：把串口收到的数据当作连续字节流，按 "'$' 开头、'\r' 或 '\n' 结尾" 切出一条条语句
 *
 * 串口每次读出来的一段数据和语句边界没有关系，状态保存在 f 里，所以：
 *   - 一段数据里有多条语句，会逐条交出；
 *   - 一条语句被拆在两次读里，前半句留在 f->line，等下一段接上；
 *   - 又收到 '$' 时：上一条已经以 "*hh" 结尾（设备没发换行）就照样交出，否则是没收完的半句，
 *     说明中间丢了数据，直接丢弃；
 *   - 第一个 '$' 之前的字节、以及超过 BD_LINE_MAX 的超长语句都丢弃。
 * 这一段处理完后，如果缓冲里的内容已经以 "*hh" 结尾，也立即交出，不用等换行
 * （有的设备语句末尾不带换行，FKI 回执这样就不会被拖到下一条语句来了才处理）。
 *
 * 参数：f       —— 断句器（调用方的静态变量，初始全 0）
 *       data, n —— 本次从串口取出的数据及长度
 *       on_line —— 每切出一条完整语句（不含换行）调用一次
 * 调用：bd_task.c 的 bd_task() 主循环，每次从串口取到数据调用一次
 */
void bd_framer_feed(bd_framer_t *f, const char *data, size_t n, bd_line_fn on_line)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = data[i];

        if (c == '$') {
            if (has_checksum_tail(f)) {
                emit_line(f, on_line);      /* 上一条是完整的，只是没换行 */
            }
            f->len = 0;                     /* 新语句开始，上一条没收完的直接丢掉 */
        } else if (f->len == 0U) {
            continue;                       /* 还没遇到 '$' */
        }
        if (c == '\r' || c == '\n') {
            emit_line(f, on_line);
            continue;
        }
        if (f->len + 1U >= sizeof(f->line)) {
            f->len = 0;                     /* 超长，整条丢掉 */
            continue;
        }
        f->line[f->len++] = c;
    }

    if (has_checksum_tail(f)) {
        emit_line(f, on_line);
    }
}

/* ------------------------------ 解析 ------------------------------ */

/*
 * 计算一条语句应有的校验和：'$' 之后、最后一个 '*' 之前（没有 '*' 就到结尾）所有字节的异或
 *
 * 例："$BDFKI,032025,TCQ,Y,0,0*77" -> 0x77
 * 参数：line —— 以 '\0' 结尾的语句，开头的 '$' 可有可无
 * 返回：校验和
 * 调用：bd_parse()（校验）、bd_task.c 的 on_line()（校验失败时在日志里给出正确的校验和，
 *       方便用串口助手手工模拟北斗语句时核对）
 */
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

/*
 * 原地解析一条语句：校验和 -> 语句类型 -> 拆字段
 *
 * 步骤：
 *   1. 必须以 '$' 开头；
 *   2. 有 '*' 时校验：'$' 和 '*' 之间所有字节异或，应等于 '*' 后的两位十六进制；
 *      通过后把 '*' 改成结束符，去掉校验和部分。没有 '*' 的语句不校验，照样接受；
 *   3. 语句头（第一个逗号之前，如 "BDPWI"）至少 4 个字符，取最后 3 个作为类型（"PWI"），
 *      所以不管发送方标识是 BD、CC 还是 GN 都能识别；
 *   4. 把每个逗号改成结束符，argv[i] 依次指向各字段（空字段是空串），最多 BD_FIELD_MAX 个。
 * 例："$BDFKI,032025,TCQ,Y,0,0*77" -> type "FKI"，argc 5，
 *     argv = { "032025", "TCQ", "Y", "0", "0" }
 * 解析后 argv 指向 line 内部，line 在使用期间不能释放或改写。
 *
 * 参数：line —— 以 '\0' 结尾的一条语句（会被改写）
 *       s    —— 输出的解析结果
 * 返回：true 解析成功；false 不以 '$' 开头、校验和错误或语句头太短
 * 调用：bd_task.c 的 on_line()
 */
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

    /* 语句头，如 "BDPWI"：最后 3 个字符是语句类型 */
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
        *p++ = '\0';                        /* 逗号改成结束符，字段从下一个字符开始 */
        s->argv[s->argc++] = p;
        p += strcspn(p, ",");
    }
    return true;
}

/* ------------------------------ 组包 ------------------------------ */

/*
 * 组一条完整语句：'$' + 按 fmt 格式化的内容 + '*' + 两位十六进制校验和 + "\r\n"
 *
 * 例：bd_build(buf, sizeof(buf), "CCICR,0,00") 得到 "$CCICR,0,00*68\r\n"，返回 16
 * 校验和 = '$' 和 '*' 之间所有字节的异或，写成大写十六进制。
 *
 * 参数：out / size —— 输出缓冲及大小（至少 8 字节）
 *       fmt, ...   —— printf 风格的格式串和参数，即 '$' 和 '*' 之间的内容
 * 返回：整条语句的长度（不含结束符）；缓冲放不下返回 -1
 * 调用：bd_task.c 的 start_send()（TCQ）、query_card()（ICR）
 */
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
    if (len < 0 || (size_t)len + 7U > size) {      /* 还要放 "*hh\r\n" 和结束符 */
        return -1;
    }

    for (i = 1; i <= len; i++) {
        sum ^= (uint8_t)out[i];
    }
    len += 1;                                       /* 加上开头的 '$' */
    len += snprintf(out + len, size - (size_t)len, "*%02X\r\n", sum);
    return len;
}

/* ------------------------------ 混编 ------------------------------ */

/*
 * 混编编码（编码类型 3）：GBK 字节 -> "A4" + 每字节两位大写十六进制
 *
 * 例：GBK 31 32 D2 BB B6 FE（"12一二"）-> "A43132D2BBB6FE"
 *
 * 参数：gbk / len  —— GBK 字节及长度
 *       out / size —— 输出缓冲及大小（需要 2 + len × 2 + 1 字节）
 * 返回：输出长度（不含结束符）；放不下返回 -1
 * 调用：bd_task.c 的 start_send()
 */
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

/*
 * 混编解码：校验 "A4" 前缀，再把后面的十六进制文本两位一组还原成 GBK 字节
 *
 * 前缀里的 'A' 大小写都认；总长度必须是偶数且每一位都是十六进制字符，否则判为不是混编数据。
 * 例："A43132D2BBB6FE" -> 31 32 D2 BB B6 FE，返回 6
 *
 * 参数：text       —— 混编文本（TCI 的第 7 个字段）
 *       out / size —— 输出缓冲及大小
 * 返回：GBK 字节数；格式不对或放不下返回 -1
 * 调用：bd_task.c 的 on_tci()
 */
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
