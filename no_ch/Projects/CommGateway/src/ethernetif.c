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

#define ETH_IRQ_PRIO        5
#define RX_TASK_STACK       512
#define RX_TASK_PRIO        (configMAX_PRIORITIES - 1)
#define TX_WAIT_MS          10

#define PHY_ADDR            0U
#define PHY_RST_GPIO        GPIOD
#define PHY_RST_PIN         GPIO_Pin_3
#define PHY_RST_CLK         RCC_AHB1Periph_GPIOD
#define PHY_REG_ID1         2U
#define PHY_REG_ID2         3U
#define PHY_LINK_WAIT_MS    5000
#define MAC_RESET_WAIT_MS   100

extern __IO ETH_DMADESCTypeDef *DMATxDescToSet;
extern __IO ETH_DMADESCTypeDef *DMARxDescToGet;

static ETH_DMADESCTypeDef s_rx_desc[ETH_RXBUFNB];
static ETH_DMADESCTypeDef s_tx_desc[ETH_TXBUFNB];
static uint32_t           s_rx_buf[ETH_RXBUFNB][ETH_RX_BUF_SIZE / 4];
static uint32_t           s_tx_buf[ETH_TXBUFNB][ETH_TX_BUF_SIZE / 4];

static struct netif      *s_netif;
static SemaphoreHandle_t  s_rx_sem;

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

static void eth_gpio_config(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOG, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    SYSCFG_ETH_MediaInterfaceConfig(SYSCFG_ETH_MediaInterface_RMII);

    pins_af_eth(GPIOA, GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_7);
    pins_af_eth(GPIOC, GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5);
    pins_af_eth(GPIOG, GPIO_Pin_11 | GPIO_Pin_13 | GPIO_Pin_14);
}

static void phy_hw_reset(void)
{
    GPIO_InitTypeDef g;

    RCC_AHB1PeriphClockCmd(PHY_RST_CLK, ENABLE);
    GPIO_ResetBits(PHY_RST_GPIO, PHY_RST_PIN);
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

static void phy_read_mode(uint32_t *speed, uint32_t *duplex)
{
    uint16_t sr = ETH_ReadPHYRegister(PHY_ADDR, PHY_SR);

    *speed  = ((sr & PHY_SPEED_STATUS) != 0U) ? ETH_Speed_10M : ETH_Speed_100M;
    *duplex = ((sr & PHY_DUPLEX_STATUS) != 0U) ? ETH_Mode_FullDuplex : ETH_Mode_HalfDuplex;
    LOG("NET: link up, %s %s duplex", (*speed == ETH_Speed_100M) ? "100M" : "10M",
        (*duplex == ETH_Mode_FullDuplex) ? "full" : "half");
}

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

bool ethernetif_hw_init(void)
{
    static bool s_mac_ready;
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
        return false;
    }
    phy_read_mode(&speed, &duplex);
    mac_dma_config(speed, duplex);
    return true;
}

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
    while ((DMATxDescToSet->Status & ETH_DMATxDesc_OWN) != 0U) {
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

static struct pbuf *low_level_input(void)
{
    __IO ETH_DMADESCTypeDef *desc   = DMARxDescToGet;
    uint32_t                 status = desc->Status;
    uint32_t                 len    = (status & ETH_DMARxDesc_FL) >> ETH_DMARxDesc_FrameLengthShift;
    struct pbuf             *p      = NULL;

    if ((status & ETH_DMARxDesc_ES) == 0U && (status & ETH_DMARxDesc_FS) != 0U &&
        (status & ETH_DMARxDesc_LS) != 0U && len > 4U && len <= ETH_RX_BUF_SIZE) {
        len -= 4U;
        p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, (const void *)desc->Buffer1Addr, (u16_t)len);
        }
    }

    desc->Status   = ETH_DMARxDesc_OWN;
    DMARxDescToGet = (ETH_DMADESCTypeDef *)desc->Buffer2NextDescAddr;
    if ((ETH->DMASR & ETH_DMASR_RBUS) != 0U) {
        ETH->DMASR   = ETH_DMASR_RBUS;
        ETH->DMARPDR = 0;
    }
    return p;
}

static void rx_task(void *arg)
{
    struct pbuf *p;

    (void)arg;
    for (;;) {
        xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(100));
        while ((DMARxDescToGet->Status & ETH_DMARxDesc_OWN) == 0U) {
            p = low_level_input();
            if (p != NULL && s_netif->input(p, s_netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
    }
}

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
