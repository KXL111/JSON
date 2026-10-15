#ifndef GBK_H
#define GBK_H

#include <stddef.h>
#include <stdint.h>

int    gbk_from_utf8(const char *utf8, uint8_t *out, size_t size);

size_t gbk_to_utf8(const uint8_t *gbk, size_t len, char *out, size_t size);

#endif
