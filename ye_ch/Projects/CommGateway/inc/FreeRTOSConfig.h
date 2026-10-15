/*
 * FreeRTOSConfig.h —— FreeRTOS 内核配置（STM32F407ZGT6，Cortex-M4F，168MHz）
 *
 * RTOS 堆（heap_4.c 的 ucHeap，64KB）由分散加载文件 MDK-ARM/CommGateway.sct 放在 CCM（0x10000000，64KB），
 * 主 SRAM（128KB）留给 lwIP、串口 / 以太网 DMA 缓冲和各模块的静态变量。
 * CCM 只有 CPU 能访问、DMA 访问不到，所以从 RTOS 堆分配的东西（任务栈、队列、信号量、lwIP 邮箱）
 * 都不能直接交给 DMA：串口发送先拷进 serial.c 的发送缓冲，网口发送先拷进 ethernetif.c 的 DMA 缓冲。
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION                        1
#define configCPU_CLOCK_HZ                          (SystemCoreClock)
#define configTICK_RATE_HZ                          ((TickType_t)1000)      /* 1 tick = 1ms */
#define configMAX_PRIORITIES                        8
#define configMINIMAL_STACK_SIZE                    ((unsigned short)128)
#define configTOTAL_HEAP_SIZE                       ((size_t)(64 * 1024))  /* 任务栈 + 队列都从这里分配，正好占满 CCM */
#define configMAX_TASK_NAME_LEN                     12
#define configUSE_16_BIT_TICKS                      0
#define configIDLE_SHOULD_YIELD                     1
#define configUSE_MUTEXES                           1
#define configUSE_RECURSIVE_MUTEXES                 1                       /* lwIP 的 sys_mutex 要用 */
#define configUSE_COUNTING_SEMAPHORES               1
#define configQUEUE_REGISTRY_SIZE                   0
#define configSUPPORT_DYNAMIC_ALLOCATION            1
#define configSUPPORT_STATIC_ALLOCATION             0

/* 钩子：栈溢出、堆不够时复位（实现见 main.c） */
#define configUSE_IDLE_HOOK                         0
#define configUSE_TICK_HOOK                         0
#define configCHECK_FOR_STACK_OVERFLOW              2
#define configUSE_MALLOC_FAILED_HOOK                1

#define configUSE_TIMERS                            0
#define configUSE_CO_ROUTINES                       0
#define configUSE_TRACE_FACILITY                    0
#define configGENERATE_RUN_TIME_STATS               0

#define INCLUDE_vTaskDelete                         1
#define INCLUDE_vTaskDelay                          1
#define INCLUDE_vTaskSuspend                        1                       /* lwIP 移植层要求 */
#define INCLUDE_xTaskGetSchedulerState              1
#define INCLUDE_xTaskGetCurrentTaskHandle           1

/* 中断优先级：STM32F4 用 4 位优先级。数值 0~1 的中断不能调用 FreeRTOS API */
#ifdef __NVIC_PRIO_BITS
#define configPRIO_BITS                             __NVIC_PRIO_BITS
#else
#define configPRIO_BITS                             4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
#define configKERNEL_INTERRUPT_PRIORITY             (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY        (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* 断言失败：关中断死循环，由看门狗复位 */
#define configASSERT(x)                             if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) {} }

/* 内核中断服务函数映射到启动文件里的向量名 */
#define vPortSVCHandler                             SVC_Handler
#define xPortPendSVHandler                          PendSV_Handler
#define xPortSysTickHandler                         SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
