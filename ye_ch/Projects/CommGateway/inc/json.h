/*
 * json —— 极简 JSON 读写，只做本项目用得到的部分
 *
 * 读：在整段文本里按键名找值（不分层级，取第一个出现的键）。
 *     终端协议里各键名互不重复，这样最简单，对格式不太规范的报文也能容错。
 * 写：按顺序追加成员，逗号和字符串转义自动处理：
 *     json_writer_t w;
 *     json_begin(&w, buf, sizeof(buf));         // {
 *     json_add_str(&w, "msg_type", "42202");    //  "msg_type":"42202"
 *     json_add_obj(&w, "content");              //  ,"content":{
 *     json_add_int(&w, "status", 0);            //     "status":0
 *     json_end_obj(&w);                         //  }
 *     len = json_end(&w);                       // }   缓冲不够返回 -1
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define JSON_MAX_DEPTH  8

/* 取字符串值（处理转义，\uXXXX 转成 UTF-8）；值是数字时按原文拷贝。
 * 键不存在或 out 放不下返回 false */
bool json_get_str(const char *js, const char *key, char *out, size_t size);

/* 取非负整数，值写成数字或数字字符串（如 "42201"）都可以 */
bool json_get_uint(const char *js, const char *key, uint64_t *out);

/* 粗查 js（len 字节，js[len] 必须是 '\0'）是不是一个完整的 JSON 对象，用于日志提示。
 * 没问题返回 NULL，否则返回一句英文的问题说明 */
const char *json_check(const char *js, size_t len);

typedef struct {
    char   *buf;
    size_t  size;
    size_t  len;
    int     depth;
    bool    need_comma[JSON_MAX_DEPTH];
    bool    overflow;
} json_writer_t;

void json_begin(json_writer_t *w, char *buf, size_t size);
void json_add_str(json_writer_t *w, const char *key, const char *val);
void json_add_int(json_writer_t *w, const char *key, long val);
void json_add_u64(json_writer_t *w, const char *key, uint64_t val);    /* 如 13 位的毫秒时间戳 */
void json_add_obj(json_writer_t *w, const char *key);
void json_end_obj(json_writer_t *w);
int  json_end(json_writer_t *w);

#endif /* JSON_H */
