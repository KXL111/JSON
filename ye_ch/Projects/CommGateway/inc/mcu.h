/*
 * mcu.h —— 芯片头文件的统一入口：工程自带的 STM32F4 标准外设库版 stm32f4xx.h
 *
 * 工程里的源文件都包含本文件，不直接 #include "stm32f4xx.h"，原因：
 *   Keil 的器件包（STM32F4xx_DFP）里也有一个同名的 stm32f4xx.h，那是 HAL 版的，
 *   没有标准库的 RCC_GetFlagStatus、GPIO_InitTypeDef 等。Keil 编辑器的实时语法检查
 *   总会把器件包的目录排在工程 Include Paths 前面（勾了 No Auto Includes 也一样），
 *   找到的是那一份，于是编辑器里满屏报 undeclared（编译器本身不受影响）。
 * 这里用相对本文件的路径直接指到工程自带的那一份，不经过 Include Paths 搜索，谁来检查都不会找错。
 * 两份头文件的防重复包含宏都是 __STM32F4xx_H，所以之后标准库内部再 #include "stm32f4xx.h"，
 * 不管找到哪一份都会被整个跳过。
 *
 * 固件库目录挪了位置时，改下面这一行的相对路径（和工程 Include Paths 里的位置一致）。
 */
#ifndef MCU_H
#define MCU_H

#include "../../../STM32F4xx_Firmware_Library/Device/stm32f4xx.h"

#endif /* MCU_H */
