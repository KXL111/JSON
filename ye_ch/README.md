# CommGateway —— 舱内终端（北斗 / 天通 通信网关）

STM32F407ZGT6 + FreeRTOS 11.2.0 + lwIP 2.2.1，Keil MDK5（ARMCC 5.06）。
硬件按正点原子探索者 STM32F4 V2.2 原理图（`refer/Explorer STM32F4_V2.2_SCH.pdf`）：8MHz 晶振，网口 LAN8720A。

终端（通信综合管理软件）走网口 UDP，北斗、天通走串口：

```text
 终端 ──UDP / JSON(1.docx)── 交换机 ── 网口 ┐  LAN8720A（RMII），复位脚 PD3
                                            本机 STM32F407ZGT6
 北斗 HBD4200 ──RS422(3.md 语句)── USART6 ──┤  PC6 发 / PC7 收，38400 8N1（外接 TTL 转 RS422 模块）
 天通 ─────────RS232(协议待定)──── USART3 ──┘  PB10 发 / PB11 收，115200 8N1（板载 SP3232，COM3）
 调试日志 ──────────────────────── USART1 ───  PA9 发 / PA10 收，115200 8N1（板载 CH340，USB_232 口）
```

- 终端 ⇄ 本机：心跳（42201/42202）、北斗短报文发送（42212/42213）、北斗短报文上报（42214/42215）
- 北斗 ⇄ 本机：读卡 ICR/ICP、信号 PWI/GSS、发报 TCQ（混编）+ 回执 FKI、收报 TCI
- 天通：串口、任务、和终端之间的通道都已就绪，协议到位后补解析

配套文档（`docs/`）：`流程框图.html`（各流程的框图）、`函数模块分析图.html`（分层、调用关系、函数清单）、
`运作流程详解.html`（逐步讲解每个流程由哪个函数完成，附日志对照）、`北斗协议语句详解.html`（读卡 / SQ / TCQ / FKI / TCI 五类语句）。
联调时怎么从调试串口确认收发和解析，见第 12 节。

## 0. 本版（05）：移植到 STM32F407ZGT6

| # | 05.txt 的要求 | 做法 | 涉及文件 |
|---|---|---|---|
| 1 | 把 04 移植到 STM32F407ZGT6，硬件参照 refer 里的原理图 | 固件库换成 ST 标准外设库 V1.4.0 + STM32F4x7 以太网驱动（从实验 55 复制，未改动）；和硬件有关的代码全部改成 STM32 外设：以太网、串口 + DMA、看门狗 IWDG、随机数 RNG、复位原因、NVIC；Keil 工程换成 STM32F407ZGTx。北斗 / 终端 / JSON / GBK 等业务代码没有改（只改了注释里的 USART 编号） | `ethernetif.c`、`serial.c`、`log.c`、`wdg.c`、`uuid.c`、`main.c`、`FreeRTOSConfig.h`、`MDK-ARM/*` |
| 2 | UDP 参考实验 55 网络通信实验 | 网口硬件全按实验 55：LAN8720A、RMII 引脚（PA1/2/7、PC1/4/5、PG11/13/14）、PD3 硬件复位、PHY 地址 0、从 31 号寄存器读速率 / 双工、MAC / DMA 参数取值相同。协议栈仍是 lwIP 2.2.1 + FreeRTOS + socket（实验 55 是 lwIP 1.4.1 裸机 RAW API），UDP 收发 `udp_link.c`、`net.c` 不变 | `ethernetif.c`、`stm32f4x7_eth_conf.h` |
| 3 | 调试打印串口为 USART1，PA9 / PA10 | `log.c` 改用 USART1（AF7），板上经 CH340 接 USB_232 口 | `log.c` |

移植时的几个决定（原因都写在对应源文件开头的注释里）：

- **北斗、天通用哪个串口**：原理图上 USART2（PA2/PA3，接 COM2 和 RS485）的 PA2 是以太网 MDIO，不能用；
  GD 版北斗用的 PD8/PD9 在这块板上是 FSMC 数据线（接 SRAM 和 LCD 接口），也不用。
  天通（RS232）用 **USART3（PB10/PB11）**，经 P10 跳线接板载 SP3232，从 COM3（DB9 公头）直接出 RS232；
  北斗（RS422）板上没有 RS422 芯片，用 **USART6（PC6/PC7，P4 排针引出）** 外接 TTL 转 RS422 模块。
- **RAM 放不下 → RTOS 堆放 CCM**：04 用了 168 KB RAM（GD32F450 有 192 KB 连续 SRAM），STM32F407 只有 128 KB 主 SRAM + 64 KB CCM。
  用分散加载文件 `MDK-ARM/CommGateway.sct` 把 64 KB 的 FreeRTOS 堆放进 CCM，主 SRAM 用 104.4 KB。
  CCM 只有 CPU 能访问，DMA 访问不到，所以 DMA 用的缓冲（以太网描述符和收发缓冲、串口收发缓冲）都是普通全局变量，留在主 SRAM。
- **网口初始化不再忙等**：ST 驱动的 `ETH_Init()` 等链路 / 自协商时是忙等（不插网线要转十几秒），START 任务一直占着 CPU，
  优先级最低的看门狗任务得不到运行，系统会被看门狗复位。现在 PHY 复位、等链路、读协商结果都在 `ethernetif.c` 里用 `vTaskDelay()` 等，
  再按协商结果直接写 MAC / DMA 寄存器；开机日志会打印 PHY 的 ID 和协商出的速率 / 双工。
- **看门狗**：FWDGT → IWDG（LSI 约 32 kHz、64 分频、重装值 2000，约 4 s）；**随机数**：TRNG → RNG（时钟取 PLLQ 的 48 MHz）。
- **时钟**：`system_stm32f4xx.c` 按 8 MHz 外部晶振配成 168 MHz（AHB 168、APB2 84、APB1 42 MHz），工程里也定义了 `HSE_VALUE=8000000`。

## 0.1 04 版相对 03 的改动（功能部分，本版沿用）

| # | 04.txt 的要求 | 做法 | 涉及文件 |
|---|---|---|---|
| 1 | 北斗用 RS422、天通用 RS232，抛弃 UDP，只有终端还用 UDP | 新增串口驱动 `serial.c`（DMA 循环接收 + 空闲中断，DMA 发送）；北斗、天通任务改用串口；`app_config.h` 删掉北斗/天通的 IP 和端口，改成波特率；北斗、天通不依赖网络，上电直接启动 | `serial.c/h`、`bd_task.c`、`tt_task.c`、`main.c`、`app_config.h` |
| 2 | 北斗、天通上传数据组 JSON 上传，msg_id、taskid 自己生成标准 UUID | 新增 `uuid.c`：RFC 4122 第 4 版 UUID，随机数取自芯片的真随机数发生器（本版是 RNG）；主动上报（42214 / 42210）的 msg_id、taskid **各自独立生成**（03 是 taskid = msg_id + 1）；终端请求缺 ID 时也用 UUID 补 | `uuid.c/h`、`term_task.c` |
| 3 | 数据存储要不要换成环形缓冲区 / 环形队列 | 串口接收**必须**用，已实现；其余不需要或已经是，理由见第 4 节 | `serial.c` |
| 4 | 参照 09 测试记录 test.txt | 逐项对照见第 5.3 节：UUID 方式、毫秒时间戳已对齐；和接口文档冲突的两处保留文档写法，各留了一处开关 | `term_task.c`、`msg_types.h`、`app_config.h` |
| 5 | 删除旧的 md、html，全部改为最新，新增代码加注释 | 旧文档全部删除后重写（本文件和 `docs/` 三个 html）；新增/改动的函数都有说明注释 | — |

另外几处顺带的改进：

- **北斗 status 增加"异常"**（1122.docx 新增的取值 2）：串口 5 秒收不到任何有效语句（线断、设备没电）报 2，
  有数据但没信号报 1（离线），见第 6 节。
- **本机时间改成 64 位毫秒**：03 用 32 位 tick 算秒，开机 49.7 天后会跳变；现在用内核的回绕计数拼成 64 位，不会回绕。
- **联调打印**：终端收发的 JSON 原文、JSON 完整性检查和解析结果，北斗串口收发的语句原文和 FKI / TCI 的解析结果，
  都打在调试串口上（`log_text()` 完整打印长报文），开关见第 2 节，怎么看见第 12 节。

## 1. 目录

```text
05/ye_ch/
├─ Projects/CommGateway/
│  ├─ inc/  src/               工程源码（见第 3 节）
│  ├─ MDK-ARM/CommGateway.uvprojx
│  │  ├─ CommGateway.sct       分散加载文件：FreeRTOS 堆放 CCM，其余放主 SRAM
│  │  ├─ output/CommGateway.hex
│  │  └─ list/CommGateway.map  内存分布
│  └─ readme.txt
├─ docs/
│  ├─ 流程框图.html            启动、终端/北斗/天通主循环、串口收发、短报文收发、SQ/status、看门狗
│  ├─ 函数模块分析图.html      分层、任务与通道、函数调用关系、跨模块调用表、函数清单、数据结构
│  ├─ 运作流程详解.html        逐图讲解：每步由哪个函数完成、调用树、数据跟踪、函数索引、日志对照
│  └─ 北斗协议语句详解.html    读卡 ICR/ICP、SQ（PWI/GSS）、TCQ、FKI、TCI 各为了什么、怎么收发
├─ STM32F4xx_Firmware_Library/ ST 标准外设库 V1.4.0，从实验 55 复制（未改动）
│  ├─ CMSIS/                   Cortex-M4 内核头文件
│  ├─ Device/                  stm32f4xx.h（HSE 8MHz）、system_stm32f4xx.c（168MHz）、启动文件 startup_stm32f40_41xxx.s
│  ├─ STM32F4xx_StdPeriph_Driver/
│  └─ STM32F4x7_ETH_Driver/    以太网 MAC/DMA 驱动
├─ Third_party/
│  ├─ lwip-2.2.1/              lwIP 源码 + 官方 arch 头文件 + 官方 FreeRTOS 移植 sys_arch.c（port/STM32F4xx，内容和 04 的 port/GD32F4xx 相同）
│  └─ FreeRTOS-Kernel-11.2.0/  只保留 Cortex-M4F 需要的文件（和 04 相同）
├─ tools/gen_gbk_table.ps1     生成 GBK 对照表 gbk_table.c（一次性工具，结果已在工程里）
├─ design.txt                  北斗状态字段的设计说明（SQ / CD / maxlength）
├─ 04.txt                      04 版需求（功能）
└─ 05.txt                      本版需求（移植到 STM32F407ZGT6）
```

`05/no_ch/` 是同一份工程去掉注释的版本（源码逐行对应，只是没有注释），不带文档。

## 2. 现场配置：只改 `inc/app_config.h`

| 配置项 | 含义 | 默认值 |
|---|---|---|
| `CFG_LOCAL_IP / NETMASK / GATEWAY / MAC` | 本机网络参数 | 10.9.0.57 / 255.255.255.0 / 10.9.0.1 |
| `CFG_TERM_LOCAL_PORT`、`CFG_TERM_IP`、`CFG_TERM_PORT` | 终端 UDP 链路 | 4220、10.9.0.24、5000 |
| `CFG_TERM_TS_MS` | 发给终端的 timestamp 单位：1 毫秒，0 秒 | 1（同 test.txt） |
| `CFG_BD_BAUDRATE` | 北斗串口波特率（8N1） | 38400（HBD4200 说明书默认值） |
| `CFG_TT_BAUDRATE` | 天通串口波特率（8N1） | 115200（协议未到，联调时确认） |
| `CFG_BD_TCQ_FREQ`、`CFG_BD_TCQ_CONFIRM` | TCQ 的频点、入站确认申请 | 2、2（同 3.md 示例） |
| `CFG_BD_FKI_TIMEOUT_MS` | TCQ 发完后等回执的时间（串口发送时间另加） | 10 s |
| `CFG_BD_ICR_RETRY_MS` | 读卡没成功时的重发间隔 | 5 s |
| `CFG_BD_SIGNAL_STALE_MS` | PWI / GSS 多久没更新就不算数 | 5 s |
| `CFG_BD_LINK_TIMEOUT_MS` | 串口多久没有有效语句判北斗异常 | 5 s |
| `CFG_LOG_ENABLE`、`CFG_LOG_BAUDRATE` | 调试串口 USART1（PA9/PA10，板载 CH340） | 开、115200 |
| `CFG_LOG_TERM_RAW` | 打印和终端之间收发的 JSON 原文（解析结果总是打印） | 开 |
| `CFG_LOG_TERM_HEARTBEAT` | 心跳 42201 / 42202 也打印（心跳频繁时可关） | 开 |
| `CFG_LOG_BD_RAW` | 打印北斗串口收发的语句原文（FKI / ICP / TCI / TCQ / ICR 等） | 开 |
| `CFG_LOG_BD_PERIODIC` | 北斗每秒自动输出的 PWI / GSS 也打印 | 关 |
| `CFG_LOG_RAW_MAX` | 一个报文最多打印多少字节，超出只提示字节数 | 2048 |

终端链路规则：终端发到 `本机IP:CFG_TERM_LOCAL_PORT`，本机发到 `CFG_TERM_IP:CFG_TERM_PORT`，只接收来自终端 IP 的包。
默认地址取自参考工程 09 的 `config.h`。

**串口用哪个 USART、哪几个引脚**属于硬件设计，在 `serial.c` 的 `k_hw[]` 表里，改板时才需要动
（换 USART 时 DMA 数据流 / 通道要按参考手册 RM0090 表 42 / 43 一起改）：

| 设备 | USART | 发 / 收 | 复用 | 接收 DMA | 发送 DMA | 探索者板上的接法 |
|---|---|---|---|---|---|---|
| 北斗（RS422） | USART6 | PC6 / PC7 | AF8 | DMA2 数据流1 通道5 | DMA2 数据流6 通道5 | P4 排针引出，外接 TTL 转 RS422 模块（3.3V 电平） |
| 天通（RS232） | USART3 | PB10 / PB11 | AF7 | DMA1 数据流1 通道4 | DMA1 数据流3 通道4 | P10 跳线帽接 COM3_RX–PB10(TX)、COM3_TX–PB11(RX)，板载 SP3232，COM3 出 RS232 |

这些引脚和以太网 RMII（PA1/2/7、PC1/4/5、PG11/13/14）、PHY 复位（PD3）、调试串口（PA9/10）都不冲突。
板上 PC6 还连着 WM8978 的 MCLK 和摄像头接口 P8 的 D0，PC7 连着 P8 的 D1：本工程不用音频和摄像头，摄像头模块不要插。
RS422、RS232 只是收发器芯片不同，两者都是全双工，MCU 侧不需要方向控制脚。

## 3. 软件结构

### 任务

| 任务 | 优先级 | 职责 | 创建时机 | 文件 |
|---|---|---|---|---|
| ETH_RX | 7 | 网卡收包交给 lwIP | 网络初始化时 | `ethernetif.c` |
| TCP/IP | 6 | lwIP 协议栈 | 网络初始化时 | `lwipopts.h` |
| TERM | 4 | 终端：UDP 收发 JSON，心跳回复，把发报请求交给设备任务，把结果和收到的报文组 JSON 上报 | 网络就绪后（START 创建） | `term_task.c` |
| BD | 4 | 北斗：RS422 串口，读卡、算 SQ/status、TCQ 发报等 FKI、TCI 收报 | 上电即创建 | `bd_task.c` |
| TT | 3 | 天通：RS232 串口，目前按行打印收到的数据 | 上电即创建 | `tt_task.c` |
| START | 2 | 等网卡就绪（网线没插就每 2 s 重试），再创建 TERM，然后删除自己 | 上电即创建 | `main.c` |
| WDG | 1 | 看门狗监视 | 上电即创建 | `wdg.c` |

每个业务任务只操作自己的链路（TERM 的 UDP socket、BD 的 USART6、TT 的 USART3），
任务之间只通过 FreeRTOS 队列传 `text_msg_t`（`app_msg.h`），设备状态放在一张加锁的总表 `dev_status` 里，心跳回复直接从表里取数。

```text
终端 ──42212──> TERM ──bd_submit()──> BD ──$BDTCQ──> USART6 ──> 北斗
终端 <─42213─── TERM <─term_post()─── BD <─$BDFKI─── USART6 <── 北斗
终端 <─42214─── TERM <─term_post()─── BD <─$BDTCI─── USART6 <── 北斗
终端 <─42202─── TERM <─dev_status──── BD <─PWI/GSS/ICP
```

### 模块（Keil 里按这几组排列）

| 分组 | 文件 | 职责 |
|---|---|---|
| App | `main.c` | 启动、创建任务、FreeRTOS 钩子 |
| | `term_task.c` | 终端协议（1.docx）：收 JSON、组 JSON、时间基准 |
| | `bd_task.c` | 北斗业务（3.md + design.txt） |
| | `tt_task.c` | 天通（占位：串口按行打日志） |
| | `dev_status.c` | 北斗/天通状态总表 |
| Protocol | `bd_proto.c` | 北斗语句断句、拆字段、校验和、组包、混编编解码（纯函数） |
| | `json.c` | 极简 JSON 读写（含 64 位数字），收包完整性粗查 `json_check()` |
| | `gbk.c` `gbk_table.c` | UTF-8 ⇄ GBK |
| Driver | `ethernetif.c` | **lwIP 网卡驱动**：ETH 引脚/时钟、LAN8720A 复位和自协商、MAC/DMA、中断、收发，全在这一个文件 |
| | `net.c` | lwIP 协议栈初始化、静态 IP |
| | `udp_link.c` | 点对点 UDP 链路（socket 封装），现在只有终端用 |
| | `serial.c` | **设备串口驱动**：USART + DMA 循环接收环形缓冲 + 空闲中断 + DMA 发送 |
| | `uuid.c` | 标准 UUID（RFC 4122 v4），随机数来自 RNG |
| | `wdg.c` | 看门狗（IWDG + 任务心跳） |
| | `log.c` | 调试串口日志；`log_text()` 完整打印收发的报文原文（自动分行） |
| Config | `app_config.h` `msg_types.h` `FreeRTOSConfig.h` `lwipopts.h` | 配置和消息号 |
| | `mcu.h` | 芯片头文件入口：直接指到工程自带的标准库版 `stm32f4xx.h`（见第 9 节） |
| | `stm32f4x7_eth_conf.h` | ST 以太网驱动配置：PHY 状态寄存器（LAN8720A） |
| | `CommGateway.sct` | 内存分配：FreeRTOS 堆放 CCM |
| 库 | `STM32F4xx_StdPeriph` 组 | 只加了用到的外设：misc、rcc、gpio、usart、dma、syscfg、iwdg、rng、dbgmcu，加以太网驱动 stm32f4x7_eth.c |

## 4. 串口驱动与环形缓冲区（04.txt 第 3 条）

### 4.1 结论

| 数据 | 存储方式 | 要不要环形 | 理由 |
|---|---|---|---|
| **串口接收**（北斗 / 天通） | DMA 循环模式 + 环形缓冲区（北斗 4 KB，天通 1 KB） | **必须，已实现** | 串口字节随时到达、不会等人，任务这时可能正忙（组包、等锁、发 TCQ）。没有缓冲就丢字节；逐字节进中断也能做，但 38400 波特率每秒约 3800 次中断。DMA 循环写、任务读，本身就是环形缓冲区：写入零 CPU、单写单读不用加锁 |
| 串口发送 | 线性发送缓冲 + DMA 普通模式 | 不需要 | 一次只发一条完整语句，TCQ 还要等 FKI 回执才发下一条，没有排队的需求 |
| 任务间消息（TERM / BD 队列） | FreeRTOS 队列 | **已经是** | FreeRTOS 队列内部就是定长槽位的环形队列（读写指针循环移动），还自带阻塞、唤醒和互斥，自己再写一个没有好处 |
| 设备状态表 `dev_status` | 结构体 + 互斥锁 | 不需要 | 只关心最新值（覆盖写），不是数据流 |
| 北斗断句行缓冲 | 线性数组 | 不需要 | 一条语句收完就交出、清空；`bd_parse()` 要在连续内存里原地拆字段 |
| 终端 UDP 收发 | lwIP pbuf 池 + 线性缓冲 | 不需要 | 以包为单位收发，lwIP 已经有缓冲池 |

可选优化（暂不需要）：TERM 队列是 4 个定长 3.2 KB 的槽位（约 13 KB），即使报文只有几个字，也占满一个槽。
RAM 紧张时可以改成 FreeRTOS 的消息缓冲区（变长环形缓冲）。目前主 SRAM 还剩约 23 KB，没有必要。

### 4.2 接收：DMA 循环 + 空闲中断

```text
            DMA 写指针 head = 缓冲大小 − DMA 剩余计数（硬件自动前进、到末尾自动绕回）
                 ↓
 ┌───────────────────────────────────────────────┐
 │ 已读 │ 已读 │ 未读 未读 未读 未读 │  空  │  空  │   环形缓冲区 s_bd_rx[4096]
 └───────────────────────────────────────────────┘
              ↑ 读指针 rx_tail（serial_read() 每次取完往前移）
```

1. `serial_open()`：接收 DMA 设成循环模式，目标是环形缓冲区；USART 只开空闲中断。
2. 字节到达：DMA 搬进缓冲区，CPU 不参与。
3. 一串数据收完、线路空闲 1 个字节时间：进 `USART6_IRQHandler`，清标志后释放信号量。
4. `serial_read()`：先按读写指针把数据拷出来（跨过末尾时分两段拷）；没有数据就等信号量，最多 20 ms。

北斗 38400 波特率每秒约 3.8 KB，4 KB 缓冲能存 1 秒多；北斗任务每 20 ms 至少来取一次，余量 50 倍。
即使任务真的卡住超过 1 秒、旧数据被覆盖，北斗语句有校验和，被破坏的语句会被丢掉，不会被当成正确数据。

### 4.3 发送：DMA 普通模式

`serial_write()` 等上一包发完（看 DMA 数据流使能位，最多 1.5 s）→ 拷进驱动的发送缓冲 → 填长度、使能 DMA → 立即返回。
38400 下一条最长的 TCQ（混编 1000 个汉字，约 4.1 KB）要发 1.1 秒，这期间北斗任务照常收数据、喂狗；
FKI 超时也从"发完"开始算（`start_send()` 里加上了发送时间）。

## 5. JSON 组包与 UUID（04.txt 第 2、4 条）

### 5.1 谁来组包

北斗、天通任务只负责设备协议，把结果或收到的短报文做成 `text_msg_t` 用 `term_post()` 交给终端任务；
**JSON 一律由终端任务组**（`term_task.c` 的 `handle_device_msg()`），所以天通接入后不用改终端这边的代码。

| 来源 | 终端收到 | msg_id | taskid |
|---|---|---|---|
| 对请求的回复（42202 / 42213 / 42209 / 不支持的请求） | 回复 | 原样带回请求里的 | 原样带回请求里的 |
| 主动上报（42214 北斗收报 / 42210 天通收报） | 上报 | **新生成的 UUID** | **另一个新生成的 UUID** |
| 请求里缺 msg_id 或 taskid | 回复 | 缺的那个用 UUID 补 | 缺的那个用 UUID 补 |

### 5.2 UUID

`uuid_generate()` 生成 RFC 4122 第 4 版（随机型）UUID，如 `3f2a9c1e-7b4d-4a10-9c3e-5d1f00a2b7c8`：

- 16 个随机字节，第 7 字节高 4 位置 `0100`（版本 4），第 9 字节高 2 位置 `10`（变体），其余 122 位随机；
- 随机数来自芯片自带的 **RNG 真随机数发生器**（时钟 48 MHz），不同设备、每次开机都不会重复；
- RNG 起不来（时钟错误 / 种子错误）时自动改用伪随机数（种子 = 芯片唯一 ID ^ tick ^ SysTick），开机日志会打印用的是哪种。

北斗收报上报示例：

```json
{"msg_id":"5f2c942d-0049-44c1-9d61-f05010da848b","timestamp":1773186776101,"msg_type":"42214",
 "content":{"taskid":"70fd2c4a-9333-4670-a35c-e5cc27672f7d","target_id":"4217580","msg":"12一二"}}
```

### 5.3 和 09 测试记录 test.txt 的对照

| 项目 | test.txt（09 实测） | 1.docx（接口文档） | 本机（04 / 05） |
|---|---|---|---|
| 主动上报的 msg_id / taskid | 两个互相独立的随机 UUID | "唯一 ID，UUID" | 两个独立的 UUID v4 —— **已对齐** |
| 回复请求时的 msg_id / taskid | 原样带回 | 同 | 原样带回 —— 一致 |
| timestamp | 13 位毫秒 | "unix 时间戳"（示例是 10 位秒） | 默认毫秒 —— **已对齐**；`CFG_TERM_TS_MS` 置 0 改成秒 |
| 回复里失败原因的键名 | `error_msg` | `errormsg` | 按文档 `errormsg`；终端读不到时改 `msg_types.h` 的 `KEY_REPLY_ERRMSG` 一处即可 |
| 心跳 SQ / maxlength 的类型 | 数字 | 字符串 | 按文档：字符串 |
| 心跳 BD.status | 没有 | 有 | 有（0 在线 / 1 离线 / 2 异常） |
| 心跳 SQ 范围 | 0~100（旧设备的口径） | 0~4 | 0~4（design.txt） |
| 心跳 VHF 段 | 有 | 有 | 没有（本机不接 VHF） |
| VHF / MESH / 通话 / 天通开关请求 | 转给设备 2 | — | 回"请求号 + 1、status = 1、not supported" |

test.txt 和 1.docx 冲突的两处（键名、数字/字符串）保留文档写法，是因为文档是双方的接口约定；
联调时如果终端按 test.txt 的写法解析，改上面说的一处即可。

## 6. 北斗状态怎么来的（心跳里的 BD 字段）

| 字段 | 来源 | 规则 |
|---|---|---|
| `status` | 串口 + SQ | 串口超过 5 s 没有任何有效语句 → **2（异常）**；否则 SQ 为 0 → 1（离线），SQ > 0 → 0（在线）。上电后先给 5 s 等设备开机 |
| `SQ` | PWI 新体制波束锁定数 + GSS 第 1 字段（可用卫星数），北斗每秒自动输出 | 每路折算成 `min(数量, 4)` 级；两路都有就取平均（四舍五入），只有一路就用这一路；超过 5 s 没更新的不算；串口异常时为 0。每秒刷新 |
| `msg` | — | 空 |
| `CD` | ICP | 区域：第 13 字段有权限就记第 14 字段的服务频度，否则 0；全球：第 17 字段有权限就记第 18 字段，否则 0。两个都记录，**上传全球频度** |
| `maxlength` | ICP 第 15 字段（通信长度等级 1~5） | 查 design.txt 的 CN/EN 表，两个都记录，**上传 CN（汉字数）** |

上电后每 5 s 发一次 `$CCICR,0,00`，收到 ICP 后不再查询。

**PWI 为什么用新体制波束数**：design.txt 原先写的是旧体制波束锁定数（第 2 字段），
但 111.pdf（HTQ206S 手册）截图里的实际输出，旧体制数一直是 `00`，锁定的波束都报在后面的新体制数里，例如
`$BDPWI,012430.31,00,03,11,45,45,00,16,49,49,00,21,46,46,00*4B`（旧体制 0，新体制 3 个波束，载噪比 45~49）。
只取旧体制的话 SQ 永远是 0，所以改用新体制波束锁定数（`bd_task.c` 的 `on_pwi()`）。

心跳回复示例（`taskstate` 无任务时是空对象，`SQ`、`maxlength` 按协议写成字符串）：

```json
{"msg_id":"<原请求>","timestamp":1748990100123,"msg_type":"42202","content":{"taskid":"<原请求>","status":0,"errormsg":"",
 "taskstate":{"taskid":"12345678","sendprogress":50},
 "Config":{"TT":{"status":1,"msg":"","SQ":"0","maxlength":"0"},
           "BD":{"status":0,"msg":"","CD":60,"SQ":"3","maxlength":"322"}}}}
```

## 7. 短报文

**发送**：42212 → 正文 UTF-8 转 GBK → 混编（`A4` + 机内码十六进制）→
`$BDTCQ,<target_id>,2,2,3,A4...,0*hh` → 串口 DMA 发出 → 等 `$BDFKI`（发送时间 + 10 s 没回执也回失败）。
同一时间只有一条在途，其余排队（队列 2 条，满了直接回失败）。

FKI 手册没给字段表，按 111.pdf 截图识别为 `$BDFKI,时间,指令名称,执行情况,失败原因,等待时间*hh`：

| 截图里的输出 | 含义 | 回终端 42213 |
|---|---|---|
| `$BDFKI,032025,TCQ,Y,0,0*77` | TCQ 发射成功 | status 0 |
| `$BDFKI,032816,TCQ,N,1,7*6E` | 2.3.3.4 入站频度控制：原因 1 = 频度未到，7 = 还要等的秒数 | status 1，`BD send too frequent, wait 7s` |
| 其他失败原因 | 手册没列出 | status 1，`BD rejected, FKI reason x, y`（原样带回） |

OFQ / EPQ / QRQ 等指令也会出 FKI，只认指令名为 TCQ 的。串口上一包 1.5 s 内没发完时回 `BD serial busy`。

**接收**：`$BDTCI` → 编码类型 3（混编）去掉 `A4` 按十六进制还原 GBK，其他类型数据本身就是 GBK/ASCII → 转 UTF-8 →
交给终端任务组 JSON 上报 42214（`target_id` 填发信方号码，msg_id、taskid 各生成一个 UUID）。终端回的 42215 只打日志。

混编示例（3.md）：`12一二` → GBK `31 32 D2BB B6FE` → `A43132D2BBB6FE`。

**断句**：串口每次读出来的一段数据和语句边界无关，`bd_framer_feed()` 把它当字节流，按 `$` 开头、换行结尾切句；
一句分几次到达会拼起来；设备不发换行时，见到 `*hh` 校验和结尾就交出。

## 8. 看门狗

硬件用独立看门狗 IWDG（LSI 时钟，约 4 s；LSI 实际 17~47 kHz，溢出时间在 2.7~7.5 s 之间），只由 WDG 任务喂。TERM / BD / TT 启动时登记，主循环每圈报一次平安
（BD 每 20 ms、TT 每 100 ms、TERM 每 20 ms）；任何一个超过 3 s 没报，WDG 就不再喂狗，系统复位。
复位后串口第一行会打印原因（`watchdog / software / power on / reset pin`）。栈溢出、堆不够也会直接复位。
仿真器暂停时看门狗同步暂停。等网线期间 TERM 还没登记，BD / TT 照常报平安；等 PHY 链路用的是 `vTaskDelay()`，
看门狗任务照常运行，所以网线晚插也不会复位。

## 9. 编译 / 下载

1. Keil 打开 `Projects/CommGateway/MDK-ARM/CommGateway.uvprojx`，F7。
   已用 MDK 5.24 / ARMCC 5.06 全部重编验证：**0 error，0 warning**；
   Code ≈ 42.5 KB，GBK 表 ≈ 133 KB（Flash 共 176.4 KB / 1 MB）；
   主 SRAM 104.4 KB / 128 KB（lwIP 内存池 43 KB、北斗和终端的缓冲 35 KB、以太网 DMA 缓冲 15 KB、串口缓冲 10 KB），
   CCM 64 KB / 64 KB（FreeRTOS 堆）。详细分布见 `MDK-ARM/list/CommGateway.map`。
2. 器件是 STM32F407ZGTx，下载要装 Keil 的 STM32F4xx_DFP 器件包（Flash 算法 STM32F4xx_1024.FLM 在里面）。
   工程不用 RTE 组件，没装器件包也能编译（已验证），只是不能下载。
   **芯片头文件统一从 `inc/mcu.h` 包含**，不要在源文件里直接写 `#include "stm32f4xx.h"`：器件包里也有一个同名的 `stm32f4xx.h`，
   那是 HAL 版的（没有标准库的 `RCC_GetFlagStatus`、`GPIO_InitTypeDef` 等），Keil 编辑器的实时语法检查总把器件包目录排在
   工程 Include Paths 前面（勾了 No Auto Includes 也一样），找到那一份就满屏报 `use of undeclared identifier`。
   `mcu.h` 用相对路径直接指到工程自带的那一份；两份的防重复包含宏都是 `__STM32F4xx_H`，之后再包含哪一份都会被跳过。
   C/C++ 页另外勾了 **No Auto Includes**，编译时也不加器件包的头文件目录。
   编辑器里个别用到 `portYIELD_FROM_ISR`、`NVIC_SystemReset` 的行可能还有黄色警告，那是 FreeRTOS RVDS 移植层和老版 CMSIS 里
   ARMCC 专用写法（`__forceinline`、`__dsb`）编辑器不认，和编译无关。
3. 调试器默认 ST-Link（和正点原子例程一致），用 J-Link / DAP 的在 Options → Debug 里改。
4. 内存布局由 `MDK-ARM/CommGateway.sct` 决定（Options → Linker 里已取消"Use Memory Layout from Target Dialog"），
   要调整 RAM 分配改这个文件，不要改 Target 页。
5. 源码是 UTF-8，Keil 里 Edit → Configuration → Encoding 选 UTF-8，中文注释才正常显示
   （固件库是 ST / 正点原子的原文件，以太网驱动里的少量中文注释是 GBK 编码，不影响编译）。
6. 网口按探索者原理图：RMII + LAN8720A（PHY 地址 0，PD3 复位）。换 PHY 改 `ethernetif.c` 开头的 `PHY_ADDR` / `PHY_RST_xxx`
   和 `stm32f4x7_eth_conf.h` 的 `PHY_SR` 等定义；引脚不同改 `ethernetif.c` 的 `eth_gpio_config()`。

## 10. 需要现场确认的地方

1. **串口硬件**：北斗 USART6（PC6/PC7）外接 TTL 转 RS422 模块、天通 USART3（PB10/PB11）走板载 SP3232（COM3），
   是按探索者原理图上和网口不冲突的引脚选的（第 2 节）；实际板子不同就改 `serial.c` 的 `k_hw[]`。
   用探索者板联调时注意 P10 的跳线帽要插在 COM3 一侧（不是 ATK 模块一侧）。
2. **天通波特率**：协议没到，先按 115200；北斗 38400 来自 HBD4200 说明书。
3. **FKI 失败原因**：手册里只有一个失败示例 `N,1,7`，"1 = 频度未到、7 = 等待秒数"是按它所在章节推断的；
   其他原因代码实测后可在 `on_fki()` 里补充。
4. **语句头**：TCQ 用 `$BDTCQ`、ICR 用 `$CCICR`，都照 3.md 的示例；收到的语句不看发送方标识，只看后 3 个字母。
5. **开机报告 OFQ**：3.md 建议业务前先发，但设计说明里没有要求，而且会占一次发射频度，所以没有发。
   需要的话在 `on_icp()` 里补一条 `$CCOFQ,1,2`。
6. **JSON 细节**：timestamp 单位、失败原因键名、心跳字段类型和 test.txt / 1.docx 的差异见第 5.3 节，各有一处开关。
7. **本机不支持的请求**（VHF/MESH 配置、天通拨号/接听/挂断/开关）：直接回"请求号+1、status=1、not supported"；天通文本 42208 回 42209 失败。
8. **时间戳**：本机没有时钟源，用终端报文里的 `timestamp` 校准（秒、毫秒都认）；没收到终端报文前是开机时长。
9. **SQ 级数**：现在是"数量直接封顶到 4"，实测后可以在 `bd_task.c` 的 `sig_level()` 里调整。
10. **超长**：本机最多处理 1000 个汉字（2000 字节 GBK）；是否超过卡的等级上限由北斗模块判断，结果通过 FKI 回给终端。
11. **网口**：PHY 地址按实验 55 用 0；开机日志 `NET: PHY id 0007:C0Fx` 说明 MDIO 通了（LAN8720A 的 ID 是 0007:C0Fx），
    `NET: no PHY answers` 说明地址或接线不对。链路速率 / 双工只在开机时协商一次（和 04 一样），运行中换交换机端口要重启。

## 11. 接入天通

天通的串口（RS232，USART3）、任务、看门狗已经就绪，收到的数据现在按行打印在调试串口上，联调时可以直接看天通输出了什么。
协议到位后在 `tt_task.c` 里补：

1. `on_line()` 里按行内容解析：信号质量、入网状态写 `dev_status` 的 `tt`（status / SQ / maxlength）；
2. `tt_submit()` 改成入队（照 `bd_submit()`），由天通任务用 `serial_write(SERIAL_TT, ...)` 发送；
3. 发送结果、收到的短信做成 `text_msg_t`（`dev = DEV_TT`）用 `term_post()` 交给终端任务，
   终端会自动组 JSON 回 42209 / 上报 42210（msg_id、taskid 自动生成 UUID），`term_task.c` 不用改。

## 12. 调试日志：确认收发和解析（联调用）

调试串口 USART1（PA9 发），115200 8N1，探索者板上插 USB_232 口（CH340）即可。**串口助手选 UTF-8 编码**，报文里的中文才能正常显示。
开关都在 `app_config.h`（见第 2 节），默认把终端和北斗的收发原文都打出来，联调完可以关掉。

一条北斗短报文从终端下发到回复的完整日志（`<uuid>` 处实际是完整的 UUID）：

```text
[50120] TERM: rx 190 bytes: {"msg_id":"<uuid>","timestamp":1748990097,"msg_type":"42212","content":{...,"target_id":"4217580","msg":"12一二"}}
[50138] TERM: parsed msg_type 42212, msg_id <uuid>, taskid <uuid>, timestamp 1748990097
[50150] TERM: BD text parsed: target_id "4217580", msg 8 bytes UTF-8: 12一二
[50156] TERM: BD text task <uuid> -> 4217580, queued
[50160] BD: TCQ task <uuid> -> 4217580, 6 bytes GBK, 42 bytes on serial
[50168] BD: tx: $BDTCQ,4217580,2,2,3,A43132D2BBB6FE,0*09
[51302] BD: rx: $BDFKI,032025,TCQ,Y,0,0*77
[51306] BD: FKI parsed: cmd TCQ, result Y, reason 0, wait 0
[51311] BD: task <uuid> sent
[51320] TERM: BD result of task <uuid> -> terminal 42213: status 0
[51340] TERM: tx 42213, 179 bytes: {"msg_id":"<uuid>","timestamp":1748990098214,"msg_type":"42213","content":{...,"status":0,"errormsg":""}}
```

| 想确认 | 看哪一行 | 不对时会看到 |
|---|---|---|
| 网口起来没有 | 开机依次是 `NET: PHY id 0007:C0Fx at MDIO address 0`、`NET: link up, 100M full duplex`、`NET: up, ip 10.9.0.57` | `NET: ETH reset timeout (no 50MHz REF_CLK from PHY?)`：PHY 没给参考时钟（没上电 / 没复位好）；`NET: no PHY answers at MDIO address 0`：MDIO 不通或地址不对；只有 `NET: ethernet not ready (cable/PHY), retry in 2000 ms` 反复出现：网线没插好或对端没起来（每次等链路 5 s，再隔 2 s 重试） |
| 终端的包收到没有 | `TERM: rx N bytes: 原文` | 没有这一行：包没到（看 `NET:` 日志、终端发的端口）；`TERM: drop N bytes from IP:端口`：发包的 IP 不是 `CFG_TERM_IP` |
| 包是不是完整的 JSON | 没有 warning | `TERM: warning: not a valid JSON object: braces not closed (truncated?)` 等，照样继续解析 |
| 公共字段解析对没有 | `TERM: parsed msg_type …, msg_id …, taskid …, timestamp …` | 请求里没带的 ID 注明 `(missing, generated)`；没有 msg_type：`parse failed: no msg_type` |
| 号码、正文解析对没有 | `TERM: BD text parsed: target_id "…", msg N bytes UTF-8: 正文` | `TERM: BD text task … rejected: 原因` |
| 发给北斗的语句 | `BD: tx: $BDTCQ,…` / `BD: tx: $CCICR,0,00*68` | `BD: task … failed: 原因` |
| 北斗回了什么 | `BD: rx: $BDFKI,…` + `BD: FKI parsed: …`；收报是 `BD: rx: $BDTCI,…` + `BD: TCI parsed: …` + `BD: TCI from …, text …: 正文` | 10 s 没回：`failed: BD no response (FKI timeout)`；回执来晚了：`FKI for TCQ but no TCQ is waiting`；校验和错：`BD: bad sentence (checksum should be *hh …)`，后面是整条语句 |
| 北斗串口通不通 | 有 `BD: rx:` 就是通的 | `BD: status 1 -> 2 (no valid sentence on serial)`；有字节但凑不出语句（波特率、接线不对）：每 5 s 一条 `BD: N bytes received on serial but no valid sentence, …` |
| 回给终端的报文 | `TERM: tx 42213, N bytes: 原文` | `TERM: tx 42213 FAILED`：UDP 发送失败 |

说明：

- 原文里的回车换行等控制字符显示成 `\x0D\x0A` 这种形式（和 JSON 里本来就有的 `\n` 转义区分开）；
  一个报文最多打 `CFG_LOG_RAW_MAX`（2048）字节，太长的自动分成多行，续行以 4 个空格开头。
- 北斗每秒自动输出的 PWI、GSS 默认不打（每秒好几行），`CFG_LOG_BD_PERIODIC` 置 1 才打，同时打出解析出的波束数 / 卫星数；
  SQ 有变化时总会打 `BD: SQ a -> b (beams x, sats y)`。设备还每秒输出别的语句、刷屏的话，加到 `bd_task.c` 的 `k_periodic[]` 表里。
- 心跳频繁时把 `CFG_LOG_TERM_HEARTBEAT` 置 0，42201 / 42202 就不再打印。
- 日志是阻塞发送，115200 下每 1 KB 约 90 ms；最长的短报文（1000 个汉字）收、发、解析各打一遍约 0.5 秒，
  不影响功能（北斗串口缓冲能存 1 秒多，看门狗 3 秒），但联调完建议把原文开关关掉。
- 用串口助手手工模拟北斗语句时，校验和算错会看到 `checksum should be *hh`，照着改就行。
- 每条日志对应哪个函数，见 `docs/运作流程详解.html` 第 12 节。
