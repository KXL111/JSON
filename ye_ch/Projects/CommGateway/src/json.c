/*
 * json.c —— 极简 JSON 读写
 *
 * 纯函数模块，只实现终端协议用得到的部分：
 *   读：json_get_str() / json_get_uint() -> find_value() 在整段文本里按键名找值
 *       （内部用 skip_ws()、string_end()；字符串值的转义由 parse_u_escape()、hex4()、utf8_put() 还原）
 *   查：json_check() 粗查收到的包是不是完整的 JSON 对象（只用于日志提示）
 *   写：json_begin() -> json_add_str / json_add_int / json_add_u64 / json_add_obj / json_end_obj -> json_end()
 *       （内部用 put_key() 处理逗号，put_string() 处理转义，put() 追加字节并检查溢出）
 */
#include <stdio.h>
#include <string.h>
#include "json.h"

/* ------------------------------- 读 ------------------------------- */

/*
 * 跳过空白字符（空格、制表符、回车、换行）
 *
 * 返回：第一个非空白字符的位置
 * 调用：find_value()、json_get_uint()
 */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/*
 * 找字符串的结尾引号：p 指向字符串内容的第一个字符（开头引号之后）
 *
 * 遇到反斜杠时连同下一个字符一起跳过，所以转义的引号不会被当成结尾。
 *
 * 返回：结尾引号的位置；字符串没结束（遇到 '\0'）返回 NULL
 * 调用：find_value()
 */
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

/*
 * 在整段 JSON 里找键名等于 key 的成员，返回它的值的首字符
 *
 * 做法：从头依次找每个用引号括起来的字符串，跳过其内容（处理转义）；如果它后面
 * （忽略空白）紧跟 ':' 且内容恰好等于 key，它就是要找的键，返回 ':' 后第一个非空白字符。
 * 字符串值后面跟的不是 ':'，不会被误认成键；字符串内部的内容整段跳过，也不会误匹配。
 * 不区分层级，返回第一个匹配的键。终端发来的请求里各键名不重复，这样最简单，
 * 对缩进、空格不规范的报文也能容错。
 *
 * 参数：js  —— 整段 JSON 文本
 *       key —— 键名（不带引号）
 * 返回：值的首字符位置（字符串值指向开头的引号）；找不到或字符串没结束返回 NULL
 * 调用：json_get_str()、json_get_uint()
 */
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

/*
 * 解析 4 位十六进制数字（\u 转义后面的 4 位）
 *
 * 参数：p —— 指向第一位
 * 返回：0~0xFFFF；遇到非十六进制字符返回 -1
 * 调用：parse_u_escape()
 */
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

/*
 * 把一个 Unicode 码点编码成 UTF-8（1~4 字节）
 *
 * 参数：cp  —— 码点（0 ~ 0x10FFFF）
 *       out —— 至少 4 字节的输出缓冲（不写结束符）
 * 返回：写入的字节数
 * 调用：json_get_str()，把 \u 转义还原成 UTF-8
 */
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

/*
 * 解析 \uXXXX（含代理对），p 指向 'u'，成功返回码点并把 *pp 移到最后一个十六进制位
 *
 * 代理对：高代理（D800~DBFF）后面紧跟另一个 \u 转义的低代理（DC00~DFFF）时，两者合成
 * 一个基本平面以外的码点（如 emoji），*pp 一起越过两个转义；单独出现的代理按原值返回。
 * 调用者的循环随后执行 p++，正好跳到转义之后的下一个字符。
 *
 * 参数：pp —— 指向当前读取位置的指针（输入时指向 'u'）
 * 返回：码点；4 位里有非十六进制字符返回 -1
 * 调用：json_get_str()
 */
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

/*
 * 按键名取字符串值，并处理 JSON 转义
 *
 * 值是字符串时：去掉两边引号，\n \r \t \b \f 和转义的引号、反斜杠、斜杠还原成原字符，
 *   \uXXXX（含代理对）转成 UTF-8 —— 终端可能把中文写成 \u 转义，也可能直接发 UTF-8，两种都支持；
 * 值不是字符串时（如数字 1859836）：原样拷贝到下一个逗号、右括号或空白为止，
 *   所以 target_id 写成数字也能取到。
 *
 * 参数：js         —— 整段 JSON
 *       key        —— 键名
 *       out / size —— 输出缓冲及大小，结果以 '\0' 结尾
 * 返回：true 成功；键不存在、字符串没结束、转义非法或 out 放不下返回 false
 * 调用：term_task.c 的 handle_json()（msg_id、taskid）、forward_text()（target_id、msg）
 */
bool json_get_str(const char *js, const char *key, char *out, size_t size)
{
    const char *p = find_value(js, key);
    size_t      n = 0;

    if (p == NULL || size == 0) {
        return false;
    }

    if (*p != '"') {                        /* 数字等非字符串值：原样拷贝 */
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
        char   ch[4];                       /* 这一个字符还原后的字节，最多 4 字节 */
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
            default:   ch[0] = *p; break;  /* \" \\ \/ */
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

/*
 * 按键名取非负整数，值写成数字（42201）或数字字符串（"42201"）都可以
 *
 * 从值的第一个数字开始读连续数字，最多 19 位（不会超出 uint64_t），13 位的毫秒时间戳也够用。
 *
 * 参数：js  —— 整段 JSON
 *       key —— 键名
 *       out —— 输出；失败时不改动
 * 返回：true 成功；键不存在或值不以数字开头返回 false
 * 调用：term_task.c 的 handle_json()（msg_type、status）、sync_time()（timestamp）
 */
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

/*
 * 粗查一段文本是不是一个完整的 JSON 对象，只用来在日志里提示，不影响解析
 *
 * json_get_*() 是按键名查找的，报文残缺时往往照样能取到前面的字段，看不出包坏了，
 * 所以收到终端的包后先用它查一遍，有问题就在日志里写明原因。检查的内容：
 *   - 包里不能有 0x00（按字符串处理时，0x00 之后的内容都看不到）；
 *   - 去掉空白后以 '{' 开头；
 *   - 引号外的 { } [ ] 配对，字符串都有结尾引号（跳过 \" 这类转义）——不配对多半是包被截断了；
 *   - 最外层 '}' 之后除了空白没有别的内容。
 * 不检查值的写法（数字、true/false 等），这些由 json_get_*() 取值时自然判断。
 *
 * 参数：js / len —— 收到的数据及长度，js[len] 必须是 '\0'（term_task() 收包后已补上）
 * 返回：NULL 没发现问题；否则是一句英文的问题说明（常量字符串）
 * 调用：term_task.c 的 handle_json()
 */
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
                i++;                        /* 转义：连同下一个字符一起跳过 */
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if ((c == '}' || c == ']') && --depth == 0) {
            break;                          /* 最外层对象结束 */
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

/* ------------------------------- 写 ------------------------------- */

/*
 * 写入器底层追加：把 n 字节追加到缓冲末尾，并保持 '\0' 结尾
 *
 * 放不下（要给结束符留 1 字节）时不写入，只置 overflow 标志；json_end() 发现标志就返回 -1，
 * 整条报文作废。这样调用方不用每写一步都判断返回值。
 *
 * 调用：本文件所有写函数
 */
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

/*
 * 写一个带引号的 JSON 字符串，必要的字符做转义
 *
 * 需要转义的：双引号、反斜杠前面加反斜杠；换行 / 回车 / 制表写成 \n \r \t；
 * 其他 0x20 以下的控制字符写成 \u00XX。其余字节（包括 UTF-8 多字节的中文）原样输出。
 * 为了少调用 put()，连续不需要转义的一段（从 run 到 s）一次性写入。
 *
 * 参数：w —— 写入器
 *       s —— 以 '\0' 结尾的字符串
 * 调用：put_key()（写键名）、json_add_str()（写值）
 */
static void put_string(json_writer_t *w, const char *s)
{
    const char *run = s;                    /* 一段不需要转义的字符的起点 */

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

/*
 * 写成员的键名部分：必要时先写逗号，再写 "key":
 *
 * need_comma[depth] 记录当前这一层是否已经写过成员：第一个成员前不加逗号，之后每个都加。
 *
 * 调用：json_add_str()、json_add_int()、json_add_obj()
 */
static void put_key(json_writer_t *w, const char *key)
{
    if (w->need_comma[w->depth]) {
        put(w, ",", 1);
    }
    w->need_comma[w->depth] = true;
    put_string(w, key);
    put(w, ":", 1);
}

/*
 * 初始化写入器，并写入最外层的 '{'
 *
 * 参数：w          —— 写入器
 *       buf / size —— 输出缓冲及大小（内容始终以 '\0' 结尾）
 * 调用：term_task.c 的 msg_begin()
 */
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

/*
 * 在当前对象里追加一个字符串成员 "key":"val"，val 自动转义
 *
 * 调用：term_task.c 组各类报文时（msg_begin()、reply_status()、reply_heartbeat()、handle_device_msg()）
 */
void json_add_str(json_writer_t *w, const char *key, const char *val)
{
    put_key(w, key);
    put_string(w, val);
}

/*
 * 在当前对象里追加一个数字成员 "key":123
 *
 * 调用：term_task.c（status、CD、sendprogress）
 */
void json_add_int(json_writer_t *w, const char *key, long val)
{
    char num[24];
    int  n = snprintf(num, sizeof(num), "%ld", val);

    put_key(w, key);
    put(w, num, (size_t)n);
}

/*
 * 在当前对象里追加一个 64 位无符号数字成员 "key":1748990097000
 *
 * 毫秒时间戳有 13 位，超出 32 位 long 的范围。工程用的 MicroLIB 的 printf 不保证支持 %llu，
 * 所以自己转十进制：从个位开始反复除以 10，倒着写进 num[]，再从最高位开始输出。
 *
 * 调用：term_task.c 的 msg_begin()（timestamp）
 */
void json_add_u64(json_writer_t *w, const char *key, uint64_t val)
{
    char   num[21];                         /* uint64_t 最多 20 位 */
    size_t pos = sizeof(num);

    do {
        num[--pos] = (char)('0' + (int)(val % 10U));
        val /= 10U;
    } while (val != 0U);

    put_key(w, key);
    put(w, num + pos, sizeof(num) - pos);
}

/*
 * 追加一个子对象 "key":{ 并进入下一层，之后的成员都写进这个子对象，直到 json_end_obj()
 *
 * 嵌套超过 JSON_MAX_DEPTH（8）层时置溢出标志，整条报文作废。
 * 调用：term_task.c（content、taskstate、Config、TT、BD）
 */
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

/*
 * 关闭当前子对象（写 '}'）并回到上一层
 *
 * 调用：term_task.c，与 json_add_obj() 成对使用
 */
void json_end_obj(json_writer_t *w)
{
    put(w, "}", 1);
    if (w->depth > 0) {
        w->depth--;
    }
}

/*
 * 关闭最外层对象，结束写入
 *
 * 返回：整段 JSON 的长度（不含结束符）；写入过程中出现过溢出返回 -1
 * 调用：term_task.c 的 msg_send()
 */
int json_end(json_writer_t *w)
{
    put(w, "}", 1);
    return w->overflow ? -1 : (int)w->len;
}
