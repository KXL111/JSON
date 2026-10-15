#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "dev_status.h"

static dev_status_t      s_status;
static SemaphoreHandle_t s_lock;

void dev_status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.bd.status = 1;
    s_status.tt.status = 1;
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
}

dev_status_t *dev_status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_status;
}

void dev_status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void dev_status_read(dev_status_t *out)
{
    *out = *dev_status_lock();
    dev_status_unlock();
}
