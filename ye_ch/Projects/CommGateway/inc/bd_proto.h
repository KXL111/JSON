/*
 * bd_proto —— 北斗语句编解码（纯函数，不涉及串口和任务），协议见 3.md
 *
 * 语句格式：$ + 发送方标识(2) + 语句类型(3) + ,字段1,字段2... + *校验和 + \r\n
 * 校验和是 '$' 和 '*' 之间所有字节的异或，写成两位十六进制。
 */
#ifndef BD_PROTO_H
#define BD_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BD_LINE_MAX     4352    /* 一条语句最大长度：混编 1000 个汉字的 TCQ/TCI 约 4.1KB */
#define BD_FIELD_MAX    32      /* 最多拆出的字段数（ICP 有 23 个字段） */

/* 断句器：把串口收到的字节流按 '$' 开头、换行结尾切出一条条语句（一句可以分几次喂进来） */
typedef struct {
    char   line[BD_LINE_MAX];
    size_t len;
} bd_framer_t;

typedef void (*bd_line_fn)(char *line);

/* 喂入收到的数据，每凑出一条完整语句（不含换行）调用一次 on_line */
void bd_framer_feed(bd_framer_t *f, const char *data, size_t n, bd_line_fn on_line);

/* 拆分后的语句，字段指针指向原来的行缓冲 */
typedef struct {
    char  type[4];              /* 语句类型，如 "PWI"（已去掉 2 字节的发送方标识） */
    int   argc;
    char *argv[BD_FIELD_MAX];   /* argv[0] 是第 1 个字段，与协议表里的编号差 1 */
} bd_sentence_t;

/* 原地拆分一条语句（会改写 line）。格式不对或校验和错误返回 false */
bool bd_parse(char *line, bd_sentence_t *s);

/* 一条语句应有的校验和：'$' 之后、最后一个 '*' 之前所有字节的异或 */
uint8_t bd_checksum(const char *line);

/* 组一条完整语句，fmt 是 '$' 和 '*' 之间的内容，如 "CCICR,0,00"。
 * 自动加上 '$'、校验和、\r\n。返回长度，放不下返回 -1 */
int  bd_build(char *out, size_t size, const char *fmt, ...);

/* 混编（编码类型 3）：GBK 字节 <-> "A4" + 十六进制文本，例："12一二" <-> "A43132D2BBB6FE"。
 * 返回输出长度；放不下或不是混编格式返回 -1 */
int  bd_mixed_encode(const uint8_t *gbk, size_t len, char *out, size_t size);
int  bd_mixed_decode(const char *text, uint8_t *out, size_t size);

#endif /* BD_PROTO_H */
