#ifndef TT_TASK_H
#define TT_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void tt_task(void *arg);

bool tt_submit(const text_msg_t *req);

#endif
