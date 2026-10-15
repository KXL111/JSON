/*
 * wdg.c —— 看门狗：IWDG 独立看门狗 + 任务心跳监视
 *
 * 两级结构：
 *   业务任务（TERM / BD / TT）：启动时 wdg_register() 登记，主循环每圈 wdg_feed() 报平安；
 *   WDG 任务（wdg_task）：每 500ms 检查一遍，所有登记的任务都在 3 秒内报过平安，
 *                        才喂硬件狗；否则停止喂狗，约 4 秒后 IWDG 溢出复位。
 */
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"
#include "wdg.h"

#define WDG_MAX_TASKS           8
#define WDG_CHECK_MS            500     /* 监视周期 */
#define WDG_TASK_TIMEOUT_MS     3000    /* 任务超过这么久没喂，判定为卡死 */
#define IWDG_RELOAD             2000U   /* LSI(约 32kHz) / 64 = 500Hz，2000 个计数约 4 秒（最大 4095） */
#define LSI_WAIT_LOOPS          100000U /* 等 LSI 稳定的最大查询次数（168MHz 下约几毫秒，正常几十微秒就好） */

static struct {
    const char          *name;
    volatile TickType_t  last;          /* 最近一次喂狗的时刻 */
} s_tasks[WDG_MAX_TASKS];
static volatile int s_count;            /* 已登记的任务数 */

/*
 * 启动独立看门狗 IWDG：LSI 时钟 64 分频（约 500Hz），重装值 2000，约 4 秒不喂就复位
 *
 * LSI 是芯片内部的 RC 振荡器，标称 32kHz、实际 17~47kHz，所以溢出时间在 2.7~7.5 秒之间，
 * 喂狗周期 500ms，留足了余量。先打开 LSI 并等它稳定（正常几十微秒；这时看门狗还没启动，
 * 所以等待有上限，万一等不到也照常往下走，IWDG_Enable() 会强制打开 LSI），
 * 再写分频和重装值（这两个寄存器在 LSI 时钟域里更新），然后重装一次计数器、启动。
 * IWDG 一旦启动就无法关闭，从这里开始必须在 4 秒内喂一次（由 WDG 任务负责）。
 * 同时设置调试时 IWDG 随内核一起暂停，断点调试不会被复位。
 * 调用：main()，调度器启动前
 */
void wdg_init(void)
{
    uint32_t i;

    DBGMCU_APB1PeriphConfig(DBGMCU_IWDG_STOP, ENABLE);     /* 仿真器暂停时看门狗也暂停，方便断点调试 */
    RCC_LSICmd(ENABLE);
    for (i = 0; i < LSI_WAIT_LOOPS && RESET == RCC_GetFlagStatus(RCC_FLAG_LSIRDY); i++) {
    }
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(IWDG_RELOAD);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/*
 * 登记一个需要监视的任务，返回它的编号（之后用来喂狗）
 *
 * 登记时把"最近一次喂狗"设为当前时刻，所以登记后有 3 秒时间进入主循环。
 * 用临界区保护 s_count，几个任务同时登记也不会冲突。最多 WDG_MAX_TASKS（8）个，
 * 超出时 configASSERT 停机，随后由硬件看门狗复位。
 *
 * 参数：name —— 任务名，卡死时打印在日志里（必须是常量字符串）
 * 返回：编号 0 ~ WDG_MAX_TASKS-1
 * 调用：term_task()、bd_task()、tt_task() 的开头
 */
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

/*
 * 任务报平安：记录当前 tick
 *
 * 只写一个 32 位数，在 Cortex-M4 上是原子操作，不用加锁。
 *
 * 参数：id —— wdg_register() 返回的编号，越界时忽略
 * 调用：各业务任务主循环每圈一次（打开链路重试期间也会调用）
 */
void wdg_feed(int id)
{
    if (id >= 0 && id < WDG_MAX_TASKS) {
        s_tasks[id].last = xTaskGetTickCount();
    }
}

/*
 * 看门狗监视任务（优先级 1，只比空闲任务高）
 *
 * 每 500ms 检查一遍登记过的任务：都在 3 秒内报过平安就喂硬件狗（IWDG_ReloadCounter）；
 * 有任务超时就打印它的名字并停止喂狗，约 4 秒后 IWDG 溢出，芯片复位，
 * 重启后开机日志显示 "reset by watchdog"。若卡住的任务在溢出前恢复，下一轮检查又会正常喂狗。
 * 优先级设成最低是有意的：某个高优先级任务死循环占满 CPU 时，WDG 任务得不到运行，
 * 同样会停止喂狗而复位。
 *
 * 参数：arg —— 未使用
 * 创建：main()
 */
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
