#include <string.h>
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "log.h"
#include "serial.h"

#define SERIAL_IRQ_PRIO     6

#define BD_RX_SIZE          4096
#define BD_TX_SIZE          4400
#define TT_RX_SIZE          1024
#define TT_TX_SIZE          512

typedef struct {
    const char         *name;
    USART_TypeDef      *usart;
    void              (*usart_clk_cmd)(uint32_t periph, FunctionalState state);
    uint32_t            usart_clk;
    IRQn_Type           irq;
    GPIO_TypeDef       *gpio;
    uint32_t            gpio_clk;
    uint8_t             af;
    uint16_t            tx_pin;
    uint8_t             tx_src;
    uint16_t            rx_pin;
    uint8_t             rx_src;
    uint32_t            dma_clk;
    DMA_Stream_TypeDef *rx_stream;
    DMA_Stream_TypeDef *tx_stream;
    uint32_t            dma_ch;
    uint32_t            rx_flags;
    uint32_t            tx_flags;
} serial_hw_t;

static const serial_hw_t k_hw[SERIAL_COUNT] = {
    [SERIAL_BD] = { "BD", USART6, RCC_APB2PeriphClockCmd, RCC_APB2Periph_USART6, USART6_IRQn,
                    GPIOC, RCC_AHB1Periph_GPIOC, GPIO_AF_USART6,
                    GPIO_Pin_6, GPIO_PinSource6, GPIO_Pin_7, GPIO_PinSource7,
                    RCC_AHB1Periph_DMA2, DMA2_Stream1, DMA2_Stream6, DMA_Channel_5,
                    DMA_FLAG_FEIF1 | DMA_FLAG_DMEIF1 | DMA_FLAG_TEIF1 | DMA_FLAG_HTIF1 | DMA_FLAG_TCIF1,
                    DMA_FLAG_FEIF6 | DMA_FLAG_DMEIF6 | DMA_FLAG_TEIF6 | DMA_FLAG_HTIF6 | DMA_FLAG_TCIF6 },
    [SERIAL_TT] = { "TT", USART3, RCC_APB1PeriphClockCmd, RCC_APB1Periph_USART3, USART3_IRQn,
                    GPIOB, RCC_AHB1Periph_GPIOB, GPIO_AF_USART3,
                    GPIO_Pin_10, GPIO_PinSource10, GPIO_Pin_11, GPIO_PinSource11,
                    RCC_AHB1Periph_DMA1, DMA1_Stream1, DMA1_Stream3, DMA_Channel_4,
                    DMA_FLAG_FEIF1 | DMA_FLAG_DMEIF1 | DMA_FLAG_TEIF1 | DMA_FLAG_HTIF1 | DMA_FLAG_TCIF1,
                    DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 | DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3 },
};

typedef struct {
    uint8_t          *rx_buf;
    size_t            rx_size;
    size_t            rx_tail;
    uint8_t          *tx_buf;
    size_t            tx_size;
    SemaphoreHandle_t rx_sem;
    bool              opened;
} serial_port_t;

static uint8_t s_bd_rx[BD_RX_SIZE];
static uint8_t s_bd_tx[BD_TX_SIZE];
static uint8_t s_tt_rx[TT_RX_SIZE];
static uint8_t s_tt_tx[TT_TX_SIZE];

static serial_port_t s_port[SERIAL_COUNT] = {
    [SERIAL_BD] = { s_bd_rx, BD_RX_SIZE, 0, s_bd_tx, BD_TX_SIZE, NULL, false },
    [SERIAL_TT] = { s_tt_rx, TT_RX_SIZE, 0, s_tt_tx, TT_TX_SIZE, NULL, false },
};

static void gpio_config(const serial_hw_t *hw)
{
    GPIO_InitTypeDef g;

    RCC_AHB1PeriphClockCmd(hw->gpio_clk, ENABLE);
    GPIO_PinAFConfig(hw->gpio, hw->tx_src, hw->af);
    GPIO_PinAFConfig(hw->gpio, hw->rx_src, hw->af);

    g.GPIO_Pin   = hw->tx_pin | hw->rx_pin;
    g.GPIO_Mode  = GPIO_Mode_AF;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(hw->gpio, &g);
}

static void dma_rx_config(const serial_hw_t *hw, const serial_port_t *p)
{
    DMA_InitTypeDef d;

    DMA_DeInit(hw->rx_stream);
    DMA_StructInit(&d);
    d.DMA_Channel            = hw->dma_ch;
    d.DMA_PeripheralBaseAddr = (uint32_t)&hw->usart->DR;
    d.DMA_Memory0BaseAddr    = (uint32_t)p->rx_buf;
    d.DMA_DIR                = DMA_DIR_PeripheralToMemory;
    d.DMA_BufferSize         = (uint32_t)p->rx_size;
    d.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    d.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    d.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    d.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    d.DMA_Mode               = DMA_Mode_Circular;
    d.DMA_Priority           = DMA_Priority_High;
    d.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    DMA_Init(hw->rx_stream, &d);
    DMA_ClearFlag(hw->rx_stream, hw->rx_flags);
    DMA_Cmd(hw->rx_stream, ENABLE);
}

static void dma_tx_config(const serial_hw_t *hw, const serial_port_t *p)
{
    DMA_InitTypeDef d;

    DMA_DeInit(hw->tx_stream);
    DMA_StructInit(&d);
    d.DMA_Channel            = hw->dma_ch;
    d.DMA_PeripheralBaseAddr = (uint32_t)&hw->usart->DR;
    d.DMA_Memory0BaseAddr    = (uint32_t)p->tx_buf;
    d.DMA_DIR                = DMA_DIR_MemoryToPeripheral;
    d.DMA_BufferSize         = 1;
    d.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    d.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    d.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    d.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    d.DMA_Mode               = DMA_Mode_Normal;
    d.DMA_Priority           = DMA_Priority_Medium;
    d.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    DMA_Init(hw->tx_stream, &d);
}

static void usart_config(const serial_hw_t *hw, uint32_t baudrate)
{
    USART_InitTypeDef u;
    NVIC_InitTypeDef  nvic;

    hw->usart_clk_cmd(hw->usart_clk, ENABLE);
    USART_DeInit(hw->usart);
    USART_StructInit(&u);
    u.USART_BaudRate            = baudrate;
    u.USART_WordLength          = USART_WordLength_8b;
    u.USART_StopBits            = USART_StopBits_1;
    u.USART_Parity              = USART_Parity_No;
    u.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    u.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(hw->usart, &u);
    USART_DMACmd(hw->usart, USART_DMAReq_Rx | USART_DMAReq_Tx, ENABLE);
    USART_ITConfig(hw->usart, USART_IT_IDLE, ENABLE);

    nvic.NVIC_IRQChannel                   = hw->irq;
    nvic.NVIC_IRQChannelPreemptionPriority = SERIAL_IRQ_PRIO;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
    USART_Cmd(hw->usart, ENABLE);
}

bool serial_open(serial_id_t id, uint32_t baudrate)
{
    const serial_hw_t *hw;
    serial_port_t     *p;

    if (id >= SERIAL_COUNT || baudrate == 0U) {
        return false;
    }
    hw = &k_hw[id];
    p  = &s_port[id];
    if (p->opened) {
        return true;
    }

    p->rx_sem = xSemaphoreCreateBinary();
    if (p->rx_sem == NULL) {
        return false;
    }
    p->rx_tail = 0;

    RCC_AHB1PeriphClockCmd(hw->dma_clk, ENABLE);
    gpio_config(hw);
    dma_rx_config(hw, p);
    dma_tx_config(hw, p);
    usart_config(hw, baudrate);
    p->opened = true;

    LOG("%s: serial %lu 8N1 ready", hw->name, (unsigned long)baudrate);
    return true;
}

static size_t rx_copy(const serial_hw_t *hw, serial_port_t *p, uint8_t *out, size_t size)
{
    size_t head = p->rx_size - DMA_GetCurrDataCounter(hw->rx_stream);
    size_t n    = 0;

    if (head >= p->rx_size) {
        head = 0;
    }
    while (n < size && p->rx_tail != head) {
        size_t end   = (head > p->rx_tail) ? head : p->rx_size;
        size_t chunk = end - p->rx_tail;

        if (chunk > size - n) {
            chunk = size - n;
        }
        memcpy(out + n, p->rx_buf + p->rx_tail, chunk);
        n          += chunk;
        p->rx_tail += chunk;
        if (p->rx_tail >= p->rx_size) {
            p->rx_tail = 0;
        }
    }
    return n;
}

size_t serial_read(serial_id_t id, void *buf, size_t size, uint32_t timeout_ms)
{
    serial_port_t *p;
    size_t         n;

    if (id >= SERIAL_COUNT || !s_port[id].opened) {
        return 0;
    }
    p = &s_port[id];
    n = rx_copy(&k_hw[id], p, (uint8_t *)buf, size);
    if (n == 0U && timeout_ms > 0U) {
        xSemaphoreTake(p->rx_sem, pdMS_TO_TICKS(timeout_ms));
        n = rx_copy(&k_hw[id], p, (uint8_t *)buf, size);
    }
    return n;
}

bool serial_write(serial_id_t id, const void *data, size_t len, uint32_t timeout_ms)
{
    const serial_hw_t *hw;
    serial_port_t     *p;
    TickType_t         start = xTaskGetTickCount();

    if (id >= SERIAL_COUNT || !s_port[id].opened) {
        return false;
    }
    hw = &k_hw[id];
    p  = &s_port[id];
    if (len == 0U || len > p->tx_size) {
        return false;
    }

    while (DMA_GetCmdStatus(hw->tx_stream) == ENABLE) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
            LOG("%s: serial tx busy, %u bytes not sent", hw->name, (unsigned)len);
            return false;
        }
        vTaskDelay(1);
    }

    memcpy(p->tx_buf, data, len);
    DMA_ClearFlag(hw->tx_stream, hw->tx_flags);
    DMA_SetCurrDataCounter(hw->tx_stream, (uint16_t)len);
    DMA_Cmd(hw->tx_stream, ENABLE);
    return true;
}

static void serial_irq(serial_id_t id)
{
    const serial_hw_t *hw    = &k_hw[id];
    BaseType_t         woken = pdFALSE;

    if (RESET != USART_GetITStatus(hw->usart, USART_IT_IDLE)) {
        (void)USART_ReceiveData(hw->usart);
        if (s_port[id].rx_sem != NULL) {
            xSemaphoreGiveFromISR(s_port[id].rx_sem, &woken);
        }
    }
    portYIELD_FROM_ISR(woken);
}

void USART6_IRQHandler(void)
{
    serial_irq(SERIAL_BD);
}

void USART3_IRQHandler(void)
{
    serial_irq(SERIAL_TT);
}
