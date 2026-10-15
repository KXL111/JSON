#ifndef DEV_STATUS_H
#define DEV_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include "app_msg.h"

typedef struct {
    bool     valid;
    char     id[16];
    uint16_t freq_region;
    uint16_t freq_global;
    uint8_t  len_level;
    uint16_t len_cn;
    uint16_t len_en;
} bd_card_t;

typedef struct {
    uint8_t   status;
    char      msg[32];
    uint16_t  cd;
    uint8_t   sq;
    uint16_t  maxlen;
    bd_card_t card;
} bd_status_t;

typedef struct {
    uint8_t   status;
    char      msg[32];
    uint8_t   sq;
    uint16_t  maxlen;
} tt_status_t;

typedef struct {
    char      taskid[APP_ID_LEN];
    uint8_t   progress;
} task_state_t;

typedef struct {
    bd_status_t  bd;
    tt_status_t  tt;
    task_state_t task;
} dev_status_t;

void          dev_status_init(void);
dev_status_t *dev_status_lock(void);
void          dev_status_unlock(void);
void          dev_status_read(dev_status_t *out);

#endif
