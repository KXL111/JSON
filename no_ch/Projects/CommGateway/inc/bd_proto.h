#ifndef BD_PROTO_H
#define BD_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BD_LINE_MAX     4352
#define BD_FIELD_MAX    32

typedef struct {
    char   line[BD_LINE_MAX];
    size_t len;
} bd_framer_t;

typedef void (*bd_line_fn)(char *line);

void bd_framer_feed(bd_framer_t *f, const char *data, size_t n, bd_line_fn on_line);

typedef struct {
    char  type[4];
    int   argc;
    char *argv[BD_FIELD_MAX];
} bd_sentence_t;

bool bd_parse(char *line, bd_sentence_t *s);

uint8_t bd_checksum(const char *line);

int  bd_build(char *out, size_t size, const char *fmt, ...);

int  bd_mixed_encode(const uint8_t *gbk, size_t len, char *out, size_t size);
int  bd_mixed_decode(const char *text, uint8_t *out, size_t size);

#endif
