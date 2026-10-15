CommGateway —— 舱内终端（北斗 / 天通 通信网关），STM32F407ZGT6 版
==================================================================

平台：STM32F407ZGT6（正点原子探索者 STM32F4 V2.2 原理图），8MHz 晶振，主频 168MHz
软件：FreeRTOS 11.2.0 + lwIP 2.2.1 + ST 标准外设库 V1.4.0，Keil MDK5（ARMCC 5.06）

接口
----
  终端（UDP / JSON）  网口 LAN8720A，RMII：PA1 PA2 PA7 PC1 PC4 PC5 PG11 PG13 PG14，复位 PD3，PHY 地址 0
  北斗（RS422）       USART6：PC6 发 / PC7 收，38400 8N1，外接 TTL 转 RS422 模块
  天通（RS232）       USART3：PB10 发 / PB11 收，115200 8N1，P10 跳线接板载 SP3232，从 COM3 出
  调试日志            USART1：PA9 发 / PA10 收，115200 8N1，板载 CH340（USB_232 口），串口助手选 UTF-8

编译 / 下载
-----------
  1. 打开 MDK-ARM\CommGateway.uvprojx，F7 编译（0 error，0 warning）。
  2. 下载要装 Keil 的 STM32F4xx_DFP 器件包；调试器默认 ST-Link，用别的在 Options -> Debug 里改。
  3. 内存分配由 MDK-ARM\CommGateway.sct 决定：FreeRTOS 堆（64KB）放 CCM，其余放主 SRAM（128KB）。
  4. 现场参数（IP、端口、波特率、日志开关）只改 inc\app_config.h。

详细说明见上两级目录的 README.md。
