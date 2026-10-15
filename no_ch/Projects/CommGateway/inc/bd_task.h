#ifndef BD_TASK_H
#define BD_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void bd_init(void);
void bd_task(void *arg);

bool bd_submit(const text_msg_t *req);

#endif
