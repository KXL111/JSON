/*
 * app_config.h —— 工程配置文件，现场部署只需要改这里
 *
 * 链路结构：
 *   终端（通信综合管理软件）<--UDP / JSON-->  本机  <--RS422 串口-->  北斗
 *                                               本机  <--RS232 串口-->  天通
 * 终端链路规则：终端发到 "本机IP : CFG_TERM_LOCAL_PORT"，本机只接收来自 CFG_TERM_IP 的数据；
 *               本机发到 "CFG_TERM_IP : CFG_TERM_PORT"。
 * 串口格式固定 8 位数据、1 位停止、无校验（8N1），这里只配波特率；
 * 用哪个 USART、哪几个引脚见 serial.c 的 k_hw[] 表（属于硬件设计，改板时才动）。
 * 硬件平台：STM32F407ZGT6（正点原子探索者 STM32F4 V2.2 原理图），网口 LAN8720A（RMII）。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ------------------------------ 本机 ------------------------------ */
#define CFG_LOCAL_IP            "10.9.0.57"
#define CFG_LOCAL_NETMASK       "255.255.255.0"
#define CFG_LOCAL_GATEWAY       "10.9.0.1"
#define CFG_LOCAL_MAC           { 0x02, 0x00, 0x0A, 0x09, 0x00, 0x39 }

/* ---------------- 终端（通信综合管理软件）：UDP ------------------- */
#define CFG_TERM_LOCAL_PORT     4220
#define CFG_TERM_IP             "10.9.0.24"
#define CFG_TERM_PORT           5000
#define CFG_TERM_TS_MS          1       /* 发给终端的 timestamp：1 毫秒（同 09 测试记录 test.txt），0 秒（同 1.docx 示例） */

/* ---------------- 北斗：RS422 串口（USART6，PC6 发 / PC7 收） ---------------- */
#define CFG_BD_BAUDRATE         38400U  /* HBD4200 说明书：设备默认 38400 */

/* ------------- 天通：RS232 串口（USART3，PB10 发 / PB11 收，板载 COM3） ------------- */
#define CFG_TT_BAUDRATE         115200U /* 天通协议还没到，先按常用值，联调时按设备说明书改 */

/* --------------------------- 北斗业务参数 -------------------------- */
#define CFG_BD_TCQ_FREQ         2       /* TCQ 频点 1~5（Lf0~Lf2、Lf4），3.md 示例用 2 */
#define CFG_BD_TCQ_CONFIRM      2       /* 入站确认申请：1 不需确认，2 需确认 */
#define CFG_BD_FKI_TIMEOUT_MS   10000   /* TCQ 发完后等北斗回执（FKI）的最长时间（串口发送时间另外加上） */
#define CFG_BD_ICR_RETRY_MS     5000    /* 读卡（ICR）还没成功时的重发间隔 */
#define CFG_BD_SIGNAL_STALE_MS  5000    /* PWI/GSS 超过这么久没更新，按没有信号处理 */
#define CFG_BD_LINK_TIMEOUT_MS  5000    /* 串口超过这么久没收到任何有效语句，判北斗异常（status = 2） */

/* ----------------------------- 调试串口 ---------------------------- */
#define CFG_LOG_ENABLE          1       /* 1 打开日志（USART1：PA9 发 / PA10 收，板载 CH340 转 USB），0 关闭 */
#define CFG_LOG_BAUDRATE        115200U

/*
 * 报文打印开关（联调用，确认无误后可以关掉，减少日志量）
 *   终端：收到的每个 UDP 包原文 + 解析结果，发给终端的每条 JSON 原文
 *   北斗：北斗串口收到的每条语句原文 + 关键语句的解析结果，发给北斗的每条语句原文
 * 原文里的中文是 UTF-8，串口助手选 UTF-8 编码显示。
 */
#define CFG_LOG_TERM_RAW        1       /* 1 打印和终端之间收发的 JSON 原文 */
#define CFG_LOG_TERM_HEARTBEAT  1       /* 1 心跳 42201 / 42202 也打印原文和解析结果；0 不打（心跳频繁时日志太多） */
#define CFG_LOG_BD_RAW          1       /* 1 打印北斗串口收发的语句原文（FKI / ICP / TCI / TCQ / ICR 等） */
#define CFG_LOG_BD_PERIODIC     0       /* 1 连北斗每秒自动输出的 PWI / GSS 也打印（每秒好几行），0 不打 */
#define CFG_LOG_RAW_MAX         2048    /* 一个报文最多打印多少字节，超出部分只提示字节数（115200 下 1KB 约 90ms） */

#endif /* APP_CONFIG_H */
