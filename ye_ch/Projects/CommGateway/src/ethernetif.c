/*
 * ethernetif.c —— lwIP 网卡驱动（STM32F407 ETH + LAN8720A，RMII 接口）
 *
 * 收：ETH 收到帧 -> 中断释放信号量 -> rx_task 把帧拷进 pbuf -> 交给 lwIP(tcpip_input)
 * 发：lwIP 调 low_level_output -> 拷进 DMA 发送缓冲 -> 启动发送
 * DMA 描述符和收发缓冲定义在本文件（ETH DMA 只能访问主 SRAM，不能放 CCM / 栈 / RTOS 堆），
 * 描述符链初始化、发送描述符交给 DMA 等用 ST 以太网驱动 stm32f4x7_eth.c。
 *
 * 为什么不用 ST 驱动的 ETH_Init()：它等 PHY 链路和自协商时是忙等（不插网线要转十几秒），
 * START 任务（优先级 2）一直占着 CPU，优先级 1 的看门狗任务得不到运行，系统会被看门狗复位。
 * 所以 PHY 复位、等链路、读协商结果都在本文件里用 vTaskDelay() 等，
 * 再按协商结果直接写 MAC / DMA 寄存器（mac_dma_config()，各项取值和实验 55 的 ETH_MACDMA_Config() 相同）。
 *
 * 函数调用关系：
 *   net_init()（START 任务）
 *     ├─ ethernetif_hw_init()                                          引脚、时钟、PHY、MAC/DMA 参数
 *     │    ├─ eth_gpio_config() -> pins_af_eth()
 *     │    ├─ phy_hw_reset() / mac_soft_reset() / mdc_clock_config() / phy_probe()
 *     │    └─ phy_wait_link() -> phy_read_mode() -> mac_dma_config()
 *     └─ netif_add() ──回调──> ethernetif_init()                       netif 字段、描述符、ETH_RX 任务、中断
 *   ETH_IRQHandler()（中断）──信号量──> rx_task()（ETH_RX 任务）-> low_level_input() -> tcpip_input
 *   lwIP 发送路径（业务任务或 TCP/IP 任务）-> etharp_output -> low_level_output()
 */
#include <string.h>
#include "mcu.h"
#include "stm32f4x7_eth.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/pbuf.h"
#include "netif/etharp.h"
#include "app_config.h"
#include "log.h"
#include "ethernetif.h"

#define ETH_IRQ_PRIO        5                           /* 数值不能小于 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY */
#define RX_TASK_STACK       512                         /* 字 */
#define RX_TASK_PRIO        (configMAX_PRIORITIES - 1)
#define TX_WAIT_MS          10                          /* 等发送描述符空闲的最长时间 */

/* LAN8720A：PHY 地址 0（RXER/PHYAD0 脚默认下拉），硬件复位脚 PD3（低有效），见原理图 EARTHNET 部分 */
#define PHY_ADDR            0U
#define PHY_RST_GPIO        GPIOD
#define PHY_RST_PIN         GPIO_Pin_3
#define PHY_RST_CLK         RCC_AHB1Periph_GPIOD
#define PHY_REG_ID1         2U                          /* PHY 标识寄存器 1 / 2（LAN8720A：0007 / C0Fx） */
#define PHY_REG_ID2         3U
#define PHY_LINK_WAIT_MS    5000                        /* 等链路建立 + 自协商完成的最长时间 */
#define MAC_RESET_WAIT_MS   100                         /* 等 MAC 软件复位完成的最长时间 */

/* ST 驱动里定义的"当前描述符"指针（驱动头文件没有声明） */
extern __IO ETH_DMADESCTypeDef *DMATxDescToSet;
extern __IO ETH_DMADESCTypeDef *DMARxDescToGet;

/* DMA 描述符和收发缓冲：ETH DMA 只能访问主 SRAM（0x20000000 起），这些全局变量由分散加载文件放在 RW_IRAM1。
 * 缓冲用 uint32_t 数组定义，保证 4 字节对齐（ETH_MAX_PACKET_SIZE = 1524 是 4 的倍数） */
static ETH_DMADESCTypeDef s_rx_desc[ETH_RXBUFNB];
static ETH_DMADESCTypeDef s_tx_desc[ETH_TXBUFNB];
static uint32_t           s_rx_buf[ETH_RXBUFNB][ETH_RX_BUF_SIZE / 4];
static uint32_t           s_tx_buf[ETH_TXBUFNB][ETH_TX_BUF_SIZE / 4];

static struct netif      *s_netif;              /* lwIP 网卡，rx_task 用它的 input 把帧交给协议栈 */
static SemaphoreHandle_t  s_rx_sem;             /* 中断 -> rx_task：有新帧到达 */

/* ----------------------------- 硬件 ----------------------------- */

/*
 * 把一组引脚配置成 AF11（ETH 复用功能）、推挽、100MHz、无上下拉
 *
 * 参数：port —— GPIOA / GPIOC / GPIOG
 *       pins —— 引脚位掩码（GPIO_Pin_x 相或）
 * 调用：eth_gpio_config()
 */
static void pins_af_eth(GPIO_TypeDef *port, uint16_t pins)
{
    GPIO_InitTypeDef g;
    uint8_t          src;

    g.GPIO_Pin   = pins;
    g.GPIO_Mode  = GPIO_Mode_AF;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_100MHz;
    g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    GPIO_Init(port, &g);
    for (src = 0; src < 16U; src++) {
        if ((pins & (1U << src)) != 0U) {
            GPIO_PinAFConfig(port, src, GPIO_AF_ETH);
        }
    }
}

/*
 * ETH 的 RMII 接口选择和引脚配置
 *
 * 打开 GPIOA/C/G 和 SYSCFG 时钟，先在 SYSCFG 里选 RMII 接口（要在打开 ETH 时钟、
 * 复位 MAC 之前选好），再把 9 个引脚配成 AF11（探索者 STM32F4 原理图）：
 * RMII：PA1 REF_CLK  PA2 MDIO  PA7 CRS_DV  PC1 MDC  PC4 RXD0  PC5 RXD1
 *       PG11 TX_EN   PG13 TXD0 PG14 TXD1
 * 板子的接法不同时只需要改这里（以及 PHY_RST_xxx）。
 *
 * 调用：ethernetif_hw_init()
 */
static void eth_gpio_config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOG, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    SYSCFG_ETH_MediaInterfaceConfig(SYSCFG_ETH_MediaInterface_RMII);

    pins_af_eth(GPIOA, GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_7);
    pins_af_eth(GPIOC, GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5);
    pins_af_eth(GPIOG, GPIO_Pin_11 | GPIO_Pin_13 | GPIO_Pin_14);
}

/*
 * LAN8720A 硬件复位：PD3 拉低 50ms 再拉高，然后等 50ms
 *
 * 复位结束后 PHY 按 MODE 引脚的默认配置（全能力 + 自协商）开始工作，并从 nINT/REFCLKO 脚
 * 输出 50MHz 参考时钟给 MCU 的 REF_CLK（PA1）——MAC 软件复位要靠这个时钟才能完成。
 *
 * 调用：ethernetif_hw_init()，运行在 START 任务里，可以用 vTaskDelay()
 */
static void phy_hw_reset(void)
{
    GPIO_InitTypeDef g;

    RCC_AHB1PeriphClockCmd(PHY_RST_CLK, ENABLE);
    GPIO_ResetBits(PHY_RST_GPIO, PHY_RST_PIN);         /* 先写输出值再切成输出，避免出现一个高电平毛刺 */
    g.GPIO_Pin   = PHY_RST_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_2MHz;
    g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    GPIO_Init(PHY_RST_GPIO, &g);

    vTaskDelay(pdMS_TO_TICKS(50));
    GPIO_SetBits(PHY_RST_GPIO, PHY_RST_PIN);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * MAC 软件复位（DMABMR.SR），等硬件清零 SR 位，最多 MAC_RESET_WAIT_MS
 *
 * 复位过程要靠 PHY 送来的 50MHz 参考时钟，PHY 没上电、没复位好或 REF_CLK 没接通时会超时。
 * 复位会把 MAC 的全部寄存器恢复默认值，所以 MDC 分频等配置要在它之后做。
 *
 * 返回：true 成功；false 超时
 * 调用：ethernetif_hw_init()
 */
static bool mac_soft_reset(void)
{
    int waited = 0;

    ETH_SoftwareReset();
    while (SET == ETH_GetSoftwareResetStatus()) {
        if (++waited > MAC_RESET_WAIT_MS) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/*
 * MDIO 管理接口时钟 MDC 分频：按 HCLK 选分频系数，保证 MDC 不超过 2.5MHz（同 ETH_Init()）
 * 168MHz 时选 102 分频，MDC ≈ 1.65MHz。
 *
 * 调用：ethernetif_hw_init()，MAC 软件复位之后、第一次读写 PHY 寄存器之前
 */
static void mdc_clock_config(void)
{
    RCC_ClocksTypeDef clk;
    uint32_t          cr;

    RCC_GetClocksFreq(&clk);
    if (clk.HCLK_Frequency < 35000000U) {
        cr = ETH_MACMIIAR_CR_Div16;
    } else if (clk.HCLK_Frequency < 60000000U) {
        cr = ETH_MACMIIAR_CR_Div26;
    } else if (clk.HCLK_Frequency < 100000000U) {
        cr = ETH_MACMIIAR_CR_Div42;
    } else if (clk.HCLK_Frequency < 150000000U) {
        cr = ETH_MACMIIAR_CR_Div62;
    } else {
        cr = ETH_MACMIIAR_CR_Div102;
    }
    ETH->MACMIIAR = cr;
}

/*
 * 读 PHY 的两个标识寄存器，确认 MDIO 上有 PHY 应答，并把 ID 打在日志里
 *
 * 读失败时 ST 驱动返回 0，没接 PHY 时 MDIO 被上拉读到 0xFFFF，两种都判为没有 PHY。
 *
 * 返回：true 找到 PHY
 * 调用：ethernetif_hw_init()
 */
static bool phy_probe(void)
{
    uint16_t id1 = ETH_ReadPHYRegister(PHY_ADDR, PHY_REG_ID1);
    uint16_t id2 = ETH_ReadPHYRegister(PHY_ADDR, PHY_REG_ID2);

    if ((id1 == 0x0000U && id2 == 0x0000U) || (id1 == 0xFFFFU && id2 == 0xFFFFU)) {
        LOG("NET: no PHY answers at MDIO address %u", (unsigned)PHY_ADDR);
        return false;
    }
    LOG("NET: PHY id %04X:%04X at MDIO address %u", (unsigned)id1, (unsigned)id2, (unsigned)PHY_ADDR);
    return true;
}

/*
 * 等链路建立且自协商完成：每 100ms 读一次 PHY 基本状态寄存器，最多 timeout_ms
 *
 * 链路状态位是"锁存低"的（断过一次就一直读到 0，读一次后才更新），循环读本身就解决了这个问题。
 * 用 vTaskDelay() 等，等待期间其他任务（包括看门狗任务）照常运行。
 *
 * 参数：timeout_ms —— 最长等待时间
 * 返回：true 链路已建立、自协商已完成；false 超时（多半是网线没插）
 * 调用：ethernetif_hw_init()
 */
static bool phy_wait_link(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint16_t   bsr;

    for (;;) {
        bsr = ETH_ReadPHYRegister(PHY_ADDR, PHY_BSR);
        if ((bsr & PHY_Linked_Status) != 0U && (bsr & PHY_AutoNego_Complete) != 0U) {
            return true;
        }
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/*
 * 读自协商结果：LAN8720A 的特殊状态寄存器（PHY_SR = 31）位 4:2
 *   位 2（PHY_SPEED_STATUS）  1 = 10M，0 = 100M
 *   位 4（PHY_DUPLEX_STATUS） 1 = 全双工，0 = 半双工
 * 寄存器号和位定义在 stm32f4x7_eth_conf.h，换 PHY 时改那里。
 *
 * 参数：speed  —— 输出 ETH_Speed_100M / ETH_Speed_10M
 *       duplex —— 输出 ETH_Mode_FullDuplex / ETH_Mode_HalfDuplex
 * 调用：ethernetif_hw_init()
 */
static void phy_read_mode(uint32_t *speed, uint32_t *duplex)
{
    uint16_t sr = ETH_ReadPHYRegister(PHY_ADDR, PHY_SR);

    *speed  = ((sr & PHY_SPEED_STATUS) != 0U) ? ETH_Speed_10M : ETH_Speed_100M;
    *duplex = ((sr & PHY_DUPLEX_STATUS) != 0U) ? ETH_Mode_FullDuplex : ETH_Mode_HalfDuplex;
    LOG("NET: link up, %s %s duplex", (*speed == ETH_Speed_100M) ? "100M" : "10M",
        (*duplex == ETH_Mode_FullDuplex) ? "full" : "half");
}

/*
 * 按自协商结果配置 MAC 和 DMA（MAC 软件复位后各寄存器是默认值，这里把需要的位置上）
 *
 * 取值和实验 55 的 ETH_MACDMA_Config()（即 ETH_StructInit() 默认值 + 它改的几项）一致：
 *   MACCR ：速率 / 双工按协商结果；看门狗、Jabber、96bit 帧间隔、载波侦听；不重传；
 *           不自动去 CRC（low_level_input() 自己减 4）；不用硬件校验和（lwIP 软件计算）
 *   MACFFR：接收广播帧（ARP 要用）；单播、组播完美地址过滤，不开混杂模式
 *   MACFCR：不用流控
 *   DMAOMR：收发都用存储转发；丢弃校验和错误帧；不转发错误帧和过短帧；处理第二帧
 *   DMABMR：地址对齐、固定突发、收发突发长度 32、收发轮询 2:1、收发分开设置突发长度
 *
 * 参数：speed / duplex —— phy_read_mode() 读出的结果
 * 调用：ethernetif_hw_init()
 */
static void mac_dma_config(uint32_t speed, uint32_t duplex)
{
    ETH->MACCR |= ETH_Watchdog_Enable | ETH_Jabber_Enable | ETH_InterFrameGap_96Bit |
                  ETH_CarrierSense_Enable | speed | ETH_ReceiveOwn_Enable | ETH_LoopbackMode_Disable |
                  duplex | ETH_ChecksumOffload_Disable | ETH_RetryTransmission_Disable |
                  ETH_AutomaticPadCRCStrip_Disable | ETH_BackOffLimit_10 | ETH_DeferralCheck_Disable;

    ETH->MACFFR = ETH_ReceiveAll_Disable | ETH_SourceAddrFilter_Disable | ETH_PassControlFrames_BlockAll |
                  ETH_BroadcastFramesReception_Enable | ETH_DestinationAddrFilter_Normal |
                  ETH_PromiscuousMode_Disable | ETH_MulticastFramesFilter_Perfect |
                  ETH_UnicastFramesFilter_Perfect;
    ETH->MACHTHR = 0;
    ETH->MACHTLR = 0;

    ETH->MACFCR |= ETH_ZeroQuantaPause_Disable | ETH_PauseLowThreshold_Minus4 |
                   ETH_UnicastPauseFrameDetect_Disable | ETH_ReceiveFlowControl_Disable |
                   ETH_TransmitFlowControl_Disable;
    ETH->MACVLANTR = ETH_VLANTagComparison_16Bit;

    ETH->DMAOMR |= ETH_DropTCPIPChecksumErrorFrame_Enable | ETH_ReceiveStoreForward_Enable |
                   ETH_FlushReceivedFrame_Enable | ETH_TransmitStoreForward_Enable |
                   ETH_TransmitThresholdControl_64Bytes | ETH_ForwardErrorFrames_Disable |
                   ETH_ForwardUndersizedGoodFrames_Disable | ETH_ReceiveThresholdControl_64Bytes |
                   ETH_SecondFrameOperate_Enable;

    ETH->DMABMR = ETH_AddressAlignedBeats_Enable | ETH_FixedBurst_Enable | ETH_RxDMABurstLength_32Beat |
                  ETH_TxDMABurstLength_32Beat | ETH_DMAArbitration_RoundRobin_RxTx_2_1 | ETH_DMABMR_USP;
}

/*
 * 以太网硬件初始化：引脚 -> PHY 复位 -> MAC 复位 -> 等链路 / 自协商 -> MAC / DMA 参数
 *
 * 步骤（第一次调用，或上次在前半段失败时）：
 *   1. eth_gpio_config()：RMII 接口和引脚；
 *   2. phy_hw_reset()：PD3 硬件复位 LAN8720A，让它输出 50MHz 参考时钟；
 *   3. 打开 ETH MAC / TX / RX 时钟，ETH_DeInit() 复位 ETH 外设，mac_soft_reset() 等 DMA 软件复位完成；
 *   4. mdc_clock_config() 设 MDIO 时钟，phy_probe() 确认 PHY 应答，写 BCR 启动自协商；
 * 之后每次调用（包括 net_init() 每 2s 的重试）：
 *   5. phy_wait_link()：等链路和自协商完成（最多 5s），网线没插就返回 false；
 *   6. phy_read_mode() 读协商出的速率 / 双工，mac_dma_config() 按它配置 MAC 和 DMA。
 * 全程用 vTaskDelay() 等待，不会饿死低优先级的看门狗任务。
 *
 * 返回：true 成功；false 失败（网线没插 / PHY 没应答 / 没有参考时钟），由 net_init() 每 2s 重试
 * 调用：net.c 的 net_init()，运行在 START 任务里
 */
bool ethernetif_hw_init(void)
{
    static bool s_mac_ready;                            /* 1~4 步已完成，重试时只等链路 */
    uint32_t    speed;
    uint32_t    duplex;

    if (!s_mac_ready) {
        eth_gpio_config();
        phy_hw_reset();

        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_ETH_MAC | RCC_AHB1Periph_ETH_MAC_Tx | RCC_AHB1Periph_ETH_MAC_Rx,
                               ENABLE);
        ETH_DeInit();
        if (!mac_soft_reset()) {
            LOG("NET: ETH reset timeout (no 50MHz REF_CLK from PHY?)");
            return false;
        }
        mdc_clock_config();
        if (!phy_probe()) {
            return false;
        }
        ETH_WritePHYRegister(PHY_ADDR, PHY_BCR, PHY_AutoNegotiation | PHY_Restart_AutoNegotiation);
        s_mac_ready = true;
    }

    if (!phy_wait_link(PHY_LINK_WAIT_MS)) {
        return false;                                   /* 多半是网线没插 */
    }
    phy_read_mode(&speed, &duplex);
    mac_dma_config(speed, duplex);
    return true;
}

/* ------------------------- 发送 / 接收 ------------------------- */

/*
 * lwIP 发送一帧（netif->linkoutput）：把 pbuf 链拷进当前发送描述符的缓冲，交给 DMA 发送
 *
 * 步骤：
 *   1. 帧长超过发送缓冲（ETH_TX_BUF_SIZE，一个最大帧）返回 ERR_BUF；
 *      大于 MTU 的 IP 包已由 lwIP 分片，正常到这里的帧都放得下；
 *   2. 当前描述符的 OWN 位还在（DMA 还没发完上一帧）时，每 1 tick 查一次，
 *      最多等约 TX_WAIT_MS（10ms），超时返回 ERR_IF；
 *   3. pbuf 链可能分成多段，逐段 memcpy 拼成一整帧（pbuf 可能在 CCM 里的任务栈上，
 *      只有 CPU 能读，所以必须拷进主 SRAM 的发送缓冲）；
 *   4. ETH_Prepare_Transmit_Descriptors() 设置帧长和首尾标志、把描述符交给 DMA、
 *      必要时唤醒发送 DMA，并移到下一个描述符。
 * lwIP 内核锁保证同一时刻只有一个线程调用这里。
 *
 * 参数：netif —— 未使用（只有一个网卡）
 *       p     —— 待发送的帧（含以太网头）
 * 返回：ERR_OK 成功；ERR_BUF 帧太长；ERR_IF 描述符忙或发送失败
 * 调用：lwIP 的 etharp_output() / ethernet_output()；UDP 发送时运行在调用 sendto() 的业务任务里
 *       （LWIP_TCPIP_CORE_LOCKING），ARP 应答、ping 回复等运行在 TCP/IP 任务里
 */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    struct pbuf *q;
    uint8_t     *buf;
    uint16_t     len = 0;
    int          waited = 0;

    (void)netif;
    if (p->tot_len > ETH_TX_BUF_SIZE) {
        return ERR_BUF;
    }
    while ((DMATxDescToSet->Status & ETH_DMATxDesc_OWN) != 0U) {     /* 描述符还被 DMA 占着 */
        if (++waited > TX_WAIT_MS) {
            return ERR_IF;
        }
        vTaskDelay(1);
    }

    buf = (uint8_t *)DMATxDescToSet->Buffer1Addr;
    for (q = p; q != NULL; q = q->next) {
        memcpy(buf + len, q->payload, q->len);
        len += q->len;
    }
    return (ETH_SUCCESS == ETH_Prepare_Transmit_Descriptors(len)) ? ERR_OK : ERR_IF;
}

/*
 * 从当前接收描述符取出一帧，拷进新分配的 pbuf，然后把描述符还给 DMA
 *
 * 只接收"没有错误、首尾标志都在（整帧在一个描述符里）、长度合理"的帧；接收缓冲按最大帧长
 * 分配，正常帧都满足。描述符里的帧长含 4 字节 CRC（没开自动去 CRC），这里减掉。
 * pbuf 从 PBUF_POOL 分配（16 个，每个 1536 字节，一帧一个），用完时返回 NULL，这一帧就丢掉了。
 * 无论成功与否都要把描述符的 OWN 位还给 DMA、移到下一个描述符，否则接收会卡住；
 * 如果 DMA 因为没有空描述符停下了（RBUS），清标志并写 DMARPDR 让它继续收。
 *
 * 返回：装好一帧的 pbuf；出错帧或内存不足返回 NULL
 * 调用：rx_task()，当前描述符已归 CPU（OWN = 0）时
 */
static struct pbuf *low_level_input(void)
{
    __IO ETH_DMADESCTypeDef *desc   = DMARxDescToGet;
    uint32_t                 status = desc->Status;
    uint32_t                 len    = (status & ETH_DMARxDesc_FL) >> ETH_DMARxDesc_FrameLengthShift;
    struct pbuf             *p      = NULL;

    if ((status & ETH_DMARxDesc_ES) == 0U && (status & ETH_DMARxDesc_FS) != 0U &&
        (status & ETH_DMARxDesc_LS) != 0U && len > 4U && len <= ETH_RX_BUF_SIZE) {
        len -= 4U;                                      /* 去掉 CRC */
        p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, (const void *)desc->Buffer1Addr, (u16_t)len);
        }
    }

    desc->Status   = ETH_DMARxDesc_OWN;                 /* 还给 DMA */
    DMARxDescToGet = (ETH_DMADESCTypeDef *)desc->Buffer2NextDescAddr;
    if ((ETH->DMASR & ETH_DMASR_RBUS) != 0U) {          /* DMA 因没有空描述符停下了，让它继续 */
        ETH->DMASR   = ETH_DMASR_RBUS;
        ETH->DMARPDR = 0;
    }
    return p;
}

/*
 * ETH_RX 任务（优先级 7，系统里最高）：把网卡收到的帧交给 lwIP
 *
 * 平时阻塞在信号量上，ETH_IRQHandler() 收到帧后释放信号量把它唤醒；醒来后把所有已归 CPU 的
 * （OWN = 0）接收描述符逐个取空：low_level_input() 拷成 pbuf，再经 s_netif->input（即 tcpip_input）
 * 投递到 TCP/IP 任务的邮箱，由 TCP/IP 任务做以太网 / ARP / IP / UDP 处理。投递失败（邮箱满）
 * 时在这里释放 pbuf。
 * 等信号量最多 100ms，超时也扫一遍描述符，防止漏掉中断导致接收停住。
 *
 * 参数：arg —— 未使用
 * 创建：ethernetif_init()
 */
static void rx_task(void *arg)
{
    struct pbuf *p;

    (void)arg;
    for (;;) {
        xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(100));   /* 超时也检查一遍，防止漏掉中断 */
        while ((DMARxDescToGet->Status & ETH_DMARxDesc_OWN) == 0U) {
            p = low_level_input();
            if (p != NULL && s_netif->input(p, s_netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
    }
}

/*
 * ETH 中断服务函数（启动文件向量表里的名字，NVIC 抢占优先级 5）
 *
 * 只处理接收完成（R）：释放信号量唤醒 rx_task，收包拷贝放到任务里做，中断尽量短。
 * 然后清除 R 和正常中断汇总（NIS）标志；被唤醒的任务优先级更高时，退出中断后立即切换过去。
 * 优先级数值 5 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY（2），才能调用 FromISR 系列 API。
 */
void ETH_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;

    if (SET == ETH_GetDMAITStatus(ETH_DMA_IT_R)) {
        xSemaphoreGiveFromISR(s_rx_sem, &woken);
    }
    ETH_DMAClearITPendingBit(ETH_DMA_IT_R);
    ETH_DMAClearITPendingBit(ETH_DMA_IT_NIS);
    portYIELD_FROM_ISR(woken);
}

/* --------------------------- netif ---------------------------- */

/*
 * lwIP 网卡初始化回调：由 netif_add() 调用，填写 netif 字段并启动 ETH 收发
 *
 * 步骤：
 *   1. 填 netif：名字 "st"；IP 层输出 etharp_output（先做 ARP 解析，再调 linkoutput）；
 *      链路层输出 low_level_output；MTU 1500；MAC 取 CFG_LOCAL_MAC；
 *      标志：支持广播、使用 ARP、链路已连通（硬件初始化时自协商已经成功）；
 *   2. 把 MAC 地址写进 ETH 的地址过滤寄存器；
 *   3. 初始化收发 DMA 描述符链（描述符和缓冲是本文件的 s_tx_desc / s_rx_desc 等），
 *      每个接收描述符都打开"接收完成中断"；
 *   4. 创建接收信号量和 ETH_RX 任务（rx_task）；
 *   5. 打开 DMA 正常中断汇总和接收中断，NVIC 使能 ETH 中断（优先级 5），ETH_Start() 启动 MAC / DMA 收发。
 *
 * 参数：netif —— lwIP 的网卡结构（net.c 里的静态变量 s_netif）
 * 返回：ERR_OK 成功；ERR_MEM 信号量或任务创建失败（RTOS 堆不够）
 * 调用：lwIP 的 netif_add()，由 net_init() 在持有 TCP/IP 内核锁时调用
 */
err_t ethernetif_init(struct netif *netif)
{
    static const uint8_t mac[ETHARP_HWADDR_LEN] = CFG_LOCAL_MAC;
    NVIC_InitTypeDef     nvic;
    uint32_t             i;

    netif->name[0]    = 's';
    netif->name[1]    = 't';
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu        = 1500;
    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, ETHARP_HWADDR_LEN);
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    s_netif = netif;

    ETH_MACAddressConfig(ETH_MAC_Address0, netif->hwaddr);
    ETH_DMATxDescChainInit(s_tx_desc, (uint8_t *)s_tx_buf, ETH_TXBUFNB);
    ETH_DMARxDescChainInit(s_rx_desc, (uint8_t *)s_rx_buf, ETH_RXBUFNB);
    for (i = 0; i < ETH_RXBUFNB; i++) {
        ETH_DMARxDescReceiveITConfig(&s_rx_desc[i], ENABLE);
    }

    s_rx_sem = xSemaphoreCreateBinary();
    if (s_rx_sem == NULL || pdPASS != xTaskCreate(rx_task, "ETH_RX", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL)) {
        return ERR_MEM;
    }

    ETH_DMAITConfig(ETH_DMA_IT_NIS | ETH_DMA_IT_R, ENABLE);
    nvic.NVIC_IRQChannel                   = ETH_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = ETH_IRQ_PRIO;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
    ETH_Start();
    return ERR_OK;
}
