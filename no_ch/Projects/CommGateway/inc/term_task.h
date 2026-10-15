#ifndef TERM_TASK_H
#define TERM_TASK_H

#include <stdbool.h>
#include "app_msg.h"

void term_init(void);
void term_task(void *arg);

bool term_post(const text_msg_t *msg);

#endif
