/*
 * gbk —— UTF-8 <-> GBK 转换
 *
 * 终端 JSON 里的文字是 UTF-8，北斗短报文用的是 GBK 机内码，收发时在这里转换。
 * 对照表 gbk_table.c 由 tools/gen_gbk_table.ps1 生成。
 */
#ifndef GBK_H
#define GBK_H

#include <stddef.h>
#include <stdint.h>

/* UTF-8 -> GBK。GBK 里没有的字符换成 '?'。
 * 返回写入的字节数（不含结束符）；out 放不下返回 -1 */
int    gbk_from_utf8(const char *utf8, uint8_t *out, size_t size);

/* GBK -> UTF-8，out 以 '\0' 结尾，放不下时截断。返回写入的字节数 */
size_t gbk_to_utf8(const uint8_t *gbk, size_t len, char *out, size_t size);

#endif /* GBK_H */
