#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"
#include "wdg.h"

#define WDG_MAX_TASKS           8
#define WDG_CHECK_MS            500
#define WDG_TASK_TIMEOUT_MS     3000
#define IWDG_RELOAD             2000U
#define LSI_WAIT_LOOPS          100000U

static struct {
    const char          *name;
    volatile TickType_t  last;
} s_tasks[WDG_MAX_TASKS];
static volatile int s_count;

void wdg_init(void)
{
    uint32_t i;

    DBGMCU_APB1PeriphConfig(DBGMCU_IWDG_STOP, ENABLE);
    RCC_LSICmd(ENABLE);
    for (i = 0; i < LSI_WAIT_LOOPS && RESET == RCC_GetFlagStatus(RCC_FLAG_LSIRDY); i++) {
    }
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(IWDG_RELOAD);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

int wdg_register(const char *name)
{
    int id;

    taskENTER_CRITICAL();
    id = s_count;
    if (id < WDG_MAX_TASKS) {
        s_tasks[id].name = name;
        s_tasks[id].last = xTaskGetTickCount();
        s_count = id + 1;
    }
    taskEXIT_CRITICAL();

    configASSERT(id < WDG_MAX_TASKS);
    return id;
}

void wdg_feed(int id)
{
    if (id >= 0 && id < WDG_MAX_TASKS) {
        s_tasks[id].last = xTaskGetTickCount();
    }
}

void wdg_task(void *arg)
{
    (void)arg;

    for (;;) {
        TickType_t  now   = xTaskGetTickCount();
        const char *stuck = NULL;
        int         i;

        for (i = 0; i < s_count; i++) {
            if (now - s_tasks[i].last > pdMS_TO_TICKS(WDG_TASK_TIMEOUT_MS)) {
                stuck = s_tasks[i].name;
                break;
            }
        }

        if (stuck == NULL) {
            IWDG_ReloadCounter();
        } else {
            LOG("WDG: task %s stuck, system will reset", stuck);
        }
        vTaskDelay(pdMS_TO_TICKS(WDG_CHECK_MS));
    }
}
