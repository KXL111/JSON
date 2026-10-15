/*
 * main.c —— 系统启动：硬件初始化 -> 创建任务 -> 启动调度
 *
 * 上电顺序：
 *   Reset_Handler（启动文件）-> SystemInit()（固件库，切到 8MHz 外部晶振 + PLL，主频 168MHz）
 *   -> main()        调度器启动前：日志、看门狗、UUID 随机源、状态表、两个队列，
 *                    创建 WDG / BD / TT / START 任务
 *   -> start_task()  调度器启动后：初始化网络（网线没插会一直等），再创建 TERM
 * 北斗、天通走串口，和网络无关，所以 BD / TT 一上电就开始工作（读卡、收信号），
 * 只有和终端通信的 TERM 要等网络就绪。
 *
 * 任务一览（数值越大优先级越高）：
 *   ETH_RX  7  网卡收包                    ethernetif.c
 *   TCP/IP  6  lwIP 协议栈                 lwipopts.h
 *   TERM    4  终端：UDP + JSON 协议、心跳、转发、上报     term_task.c
 *   BD      4  北斗：RS422 串口，读卡、信号、短报文        bd_task.c
 *   TT      3  天通：RS232 串口，占位                     tt_task.c
 *   START   2  等网络就绪后创建 TERM，然后自删
 *   WDG     1  看门狗监视：所有业务任务按时报平安才喂硬件狗
 */
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

#define STACK_TERM      1024            /* 单位：字（4 字节） */
#define STACK_BD        1024
#define STACK_TT        384
#define STACK_START     512
#define STACK_WDG       256

/*
 * 读取并清除 RCC 复位标志，返回本次复位原因的文字描述（写在开机第一行日志里）
 *
 * 判断顺序：独立看门狗 IWDG -> 软件复位（FreeRTOS 钩子里的 NVIC_SystemReset）-> 上电 -> 复位引脚，
 * 都不是则为 "other"。多个标志可能同时置位（例如上电时 POR 和 PIN 都会置位），按这个顺序只报最主要的一个。
 * 判断完立即清除全部标志，否则标志会一直保留到下次复位，造成误判。
 *
 * 返回：常量字符串 "watchdog" / "software" / "power on" / "reset pin" / "other"
 * 调用：main()，启动时调用一次
 */
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

/*
 * START 任务：等网络就绪后创建终端任务 TERM，然后删除自己
 *
 * 为什么单独用一个任务做网络初始化：
 *   - net_init() 里网卡初始化要等 PHY 自协商，网线没插时用 vTaskDelay() 每 2 秒重试，
 *     而 vTaskDelay() 只能在调度器运行后的任务里调用；
 *   - 等网线期间 WDG、BD、TT 照常运行，系统不会因为网线晚插而复位，北斗也不耽误读卡；
 *   - TERM 在网络就绪后才创建，一启动就能直接打开 UDP socket。
 * TERM 的优先级（4）高于 START（2），创建后会立即抢占运行。
 *
 * 参数：arg —— 未使用（创建时传入 NULL）
 * 创建：main()，优先级 PRIO_START(2)；执行完 vTaskDelete(NULL) 删除自己，栈由空闲任务回收
 */
static void start_task(void *arg)
{
    (void)arg;

    net_init();                                             /* 阻塞到网卡、lwIP 都就绪 */
    xTaskCreate(term_task, "TERM", STACK_TERM, NULL, PRIO_TERM, NULL);
    vTaskDelete(NULL);
}

/*
 * 程序入口：由启动文件在 SystemInit() 之后调用
 *
 * 这里只做不依赖网络的初始化，顺序有讲究：
 *   1. NVIC 分组设为 4 位抢占优先级、0 位子优先级 —— FreeRTOS Cortex-M 移植的要求；
 *   2. log_init()：最先打开调试串口，后面任何一步出问题都能看到日志；随后打印复位原因；
 *   3. wdg_init()：尽早启动硬件看门狗，启动过程卡死也能复位。从这里到 WDG 任务第一次
 *      喂狗不能超过约 4 秒（实际只有几毫秒）；
 *   4. uuid_init()：打开 RNG，给发往终端的 msg_id / taskid 提供随机数；
 *   5. dev_status_init() / term_init() / bd_init()：状态表的互斥锁和 TERM、BD 两个队列，
 *      必须在任何任务用到它们之前建好；
 *   6. 创建 WDG（1）、BD（4）、TT（3）、START（2）四个任务，启动调度器。
 *      BD / TT 各自在任务里打开自己的串口。
 *
 * 返回：vTaskStartScheduler() 正常不返回；只有 FreeRTOS 堆不够创建空闲任务时才会返回，
 *       此时停在死循环里，约 4 秒后由硬件看门狗复位。
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);        /* FreeRTOS 要求只用抢占优先级 */
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

    for (;;) {                                              /* 堆不够才会走到这里 */
    }
}

/* ---------------- FreeRTOS 钩子：出现就复位重启 ---------------- */

/*
 * FreeRTOS 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2：任务切换时检查栈尾的填充标记）
 *
 * 发现任一任务栈溢出就立即软件复位。此时内存可能已被破坏，而且本函数在任务切换
 * （PendSV 中断）里被调用，不能用 LOG()（要拿互斥锁），所以不打日志；
 * 重启后开机日志会显示 "reset by software"。
 *
 * 参数：task —— 溢出任务的句柄；name —— 任务名（都未使用）
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;                         /* 在调度器内部被调用，不能打日志 */
    NVIC_SystemReset();
}

/*
 * FreeRTOS 内存分配失败钩子（configUSE_MALLOC_FAILED_HOOK = 1）
 *
 * pvPortMalloc() 从 64KB 的 RTOS 堆（在 CCM 里，见 CommGateway.sct）分配失败时调用，例如创建任务、队列、信号量
 * （包括 lwIP 内部的邮箱、信号量）时堆不够。打一条日志后软件复位，
 * 重启后开机日志显示 "reset by software"。
 */
void vApplicationMallocFailedHook(void)
{
    LOG("FATAL: out of heap");
    NVIC_SystemReset();
}
