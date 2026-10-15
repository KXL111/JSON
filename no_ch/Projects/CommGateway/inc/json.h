#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define JSON_MAX_DEPTH  8

bool json_get_str(const char *js, const char *key, char *out, size_t size);

bool json_get_uint(const char *js, const char *key, uint64_t *out);

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
void json_add_u64(json_writer_t *w, const char *key, uint64_t val);
void json_add_obj(json_writer_t *w, const char *key);
void json_end_obj(json_writer_t *w);
int  json_end(json_writer_t *w);

#endif
