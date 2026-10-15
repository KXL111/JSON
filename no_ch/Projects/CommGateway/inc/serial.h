#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SERIAL_BD = 0,
    SERIAL_TT,
    SERIAL_COUNT
} serial_id_t;

bool   serial_open(serial_id_t id, uint32_t baudrate);

size_t serial_read(serial_id_t id, void *buf, size_t size, uint32_t timeout_ms);

bool   serial_write(serial_id_t id, const void *data, size_t len, uint32_t timeout_ms);

#endif
