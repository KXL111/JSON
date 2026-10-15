#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bd_task.h"
#include "dev_status.h"
#include "log.h"
#include "net.h"
#include "term_task.h"
#include "tt_task.h"
#include "uuid.h"
#include "wdg.h"

#define PRIO_TERM       4
#define PRIO_BD         4
#define PRIO_TT         3
#define PRIO_START      2
#define PRIO_WDG        1

#define STACK_TERM      1024
#define STACK_BD        1024
#define STACK_TT        384
#define STACK_START     512
#define STACK_WDG       256

static const char *reset_reason(void)
{
    const char *reason = "other";

    if (SET == RCC_GetFlagStatus(RCC_FLAG_IWDGRST)) {
        reason = "watchdog";
    } else if (SET == RCC_GetFlagStatus(RCC_FLAG_SFTRST)) {
        reason = "software";
    } else if (SET == RCC_GetFlagStatus(RCC_FLAG_PORRST)) {
        reason = "power on";
    } else if (SET == RCC_GetFlagStatus(RCC_FLAG_PINRST)) {
        reason = "reset pin";
    }
    RCC_ClearFlag();
    return reason;
}

static void start_task(void *arg)
{
    (void)arg;

    net_init();
    xTaskCreate(term_task, "TERM", STACK_TERM, NULL, PRIO_TERM, NULL);
    vTaskDelete(NULL);
}

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    log_init();
    LOG("CommGateway start, reset by %s", reset_reason());

    wdg_init();
    uuid_init();
    dev_status_init();
    term_init();
    bd_init();

    xTaskCreate(wdg_task,   "WDG",   STACK_WDG,   NULL, PRIO_WDG,   NULL);
    xTaskCreate(bd_task,    "BD",    STACK_BD,    NULL, PRIO_BD,    NULL);
    xTaskCreate(tt_task,    "TT",    STACK_TT,    NULL, PRIO_TT,    NULL);
    xTaskCreate(start_task, "START", STACK_START, NULL, PRIO_START, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    NVIC_SystemReset();
}

void vApplicationMallocFailedHook(void)
{
    LOG("FATAL: out of heap");
    NVIC_SystemReset();
}
