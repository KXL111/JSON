/*
 * serial.c —— 设备串口驱动（北斗 RS422 / 天通 RS232）
 *
 * 硬件资源都写在 k_hw[] 表里，换 USART 或引脚只改这张表（DMA 数据流 / 通道要按 RM0090 表 42/43 一起改）：
 *   北斗 USART6：PC6 发 / PC7 收（AF8），接收 DMA2 数据流1、发送 DMA2 数据流6，通道 5
 *               探索者板上 PC6/PC7 在 P4 排针上引出，外接 TTL 转 RS422 模块
 *   天通 USART3：PB10 发 / PB11 收（AF7），接收 DMA1 数据流1、发送 DMA1 数据流3，通道 4
 *               探索者板上经 P10 跳线（COM3_TX-PB11、COM3_RX-PB10）接板载 SP3232，从 COM3（DB9 公头）出 RS232
 * 这些引脚和以太网 RMII（PA1/2/7、PC1/4/5、PG11/13/14）、PHY 复位（PD3）、调试串口 USART1（PA9/10）都不冲突。
 * 板载 USART2（PA2/PA3，接 COM2 / RS485）不能用：PA2 是以太网的 MDIO。
 *
 * 函数调用关系：
 *   serial_open()  -> gpio_config() / usart_config() / dma_rx_config() / dma_tx_config()
 *   serial_read()  -> rx_copy()                          从环形缓冲区取数据，没有就等信号量
 *   serial_write()                                       等上一包发完 -> 拷贝 -> 启动 DMA
 *   USART6_IRQHandler / USART3_IRQHandler -> serial_irq()   空闲中断：释放信号量唤醒任务
 */
#include <string.h>
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "log.h"
#include "serial.h"

/* 中断优先级数值必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY（2）才能调用 FromISR API；
 * 以太网是 5，串口数据量小、有 DMA 兜底，排在它后面 */
#define SERIAL_IRQ_PRIO     6

/* 缓冲区大小（字节）。北斗 38400 波特率每秒约 3.8KB：接收缓冲 4KB 能存 1 秒多的数据，
 * 任务每 20ms 就来取一次，余量很大；发送缓冲要放得下一条最长的 TCQ（混编 1000 汉字约 4.1KB）。
 * DMA 计数器是 16 位，单次最多 65535 字节 */
#define BD_RX_SIZE          4096
#define BD_TX_SIZE          4400
#define TT_RX_SIZE          1024
#define TT_TX_SIZE          512

/* 一个串口用到的全部硬件资源 */
typedef struct {
    const char         *name;           /* 日志里显示的名字 */
    USART_TypeDef      *usart;
    void              (*usart_clk_cmd)(uint32_t periph, FunctionalState state);  /* RCC_APB1/APB2PeriphClockCmd */
    uint32_t            usart_clk;      /* RCC_APBxPeriph_USARTx */
    IRQn_Type           irq;
    GPIO_TypeDef       *gpio;           /* 收发引脚所在端口 */
    uint32_t            gpio_clk;       /* RCC_AHB1Periph_GPIOx */
    uint8_t             af;             /* 引脚复用功能号 */
    uint16_t            tx_pin;         /* GPIO_Pin_x */
    uint8_t             tx_src;         /* GPIO_PinSourcex，和 tx_pin 对应 */
    uint16_t            rx_pin;
    uint8_t             rx_src;
    uint32_t            dma_clk;        /* RCC_AHB1Periph_DMA1 / DMA2 */
    DMA_Stream_TypeDef *rx_stream;
    DMA_Stream_TypeDef *tx_stream;
    uint32_t            dma_ch;         /* DMA 请求通道号，收发两个数据流相同 */
    uint32_t            rx_flags;       /* 接收数据流的全部状态标志，启动前清除 */
    uint32_t            tx_flags;       /* 发送数据流的全部状态标志，每次发送前清除 */
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

/* 一个串口的运行状态 */
typedef struct {
    uint8_t          *rx_buf;           /* 环形缓冲区：DMA 往里写，任务从里读 */
    size_t            rx_size;
    size_t            rx_tail;          /* 读指针：任务下次从这里开始读（写指针由 DMA 计数器给出） */
    uint8_t          *tx_buf;           /* 发送缓冲：DMA 从这里发出 */
    size_t            tx_size;
    SemaphoreHandle_t rx_sem;           /* 空闲中断 -> 任务："有新数据了" */
    bool              opened;
} serial_port_t;

/* 收发缓冲是普通全局变量，在主 SRAM 里（DMA 访问不到 CCM，不能放到 RTOS 堆或任务栈上） */
static uint8_t s_bd_rx[BD_RX_SIZE];
static uint8_t s_bd_tx[BD_TX_SIZE];
static uint8_t s_tt_rx[TT_RX_SIZE];
static uint8_t s_tt_tx[TT_TX_SIZE];

static serial_port_t s_port[SERIAL_COUNT] = {
    [SERIAL_BD] = { s_bd_rx, BD_RX_SIZE, 0, s_bd_tx, BD_TX_SIZE, NULL, false },
    [SERIAL_TT] = { s_tt_rx, TT_RX_SIZE, 0, s_tt_tx, TT_TX_SIZE, NULL, false },
};

/* ------------------------------ 初始化 ------------------------------ */

/*
 * 收发引脚设为复用功能：推挽输出、上拉（线没接时 RX 保持空闲高电平，不会收到乱码）
 *
 * 调用：serial_open()
 */
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

/*
 * 接收 DMA：外设（USART 数据寄存器）-> 内存（环形缓冲区），循环模式，直接模式（不用 FIFO）
 *
 * 循环模式下计数器减到 0 会自动重装成 rx_size，地址回到缓冲区开头，所以 DMA 永远不停，
 * 当前写到的位置 = rx_size - 剩余计数。不开 DMA 中断：任务至少每 20ms 来取一次，
 * 长时间连续收数据（没有空闲中断）也不会被覆盖。
 * 使能数据流前要清掉它的全部标志，否则启动不了。
 *
 * 调用：serial_open()
 */
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

/*
 * 发送 DMA：内存（发送缓冲）-> 外设（USART 数据寄存器），普通模式，直接模式
 *
 * 这里只配置固定参数（地址固定是本口的发送缓冲），不使能数据流；每次 serial_write() 再填长度、使能。
 * 普通模式下传完最后一个字节，硬件自动清掉数据流使能位（EN），serial_write() 靠这一位判断是否发完。
 *
 * 调用：serial_open()
 */
static void dma_tx_config(const serial_hw_t *hw, const serial_port_t *p)
{
    DMA_InitTypeDef d;

    DMA_DeInit(hw->tx_stream);
    DMA_StructInit(&d);
    d.DMA_Channel            = hw->dma_ch;
    d.DMA_PeripheralBaseAddr = (uint32_t)&hw->usart->DR;
    d.DMA_Memory0BaseAddr    = (uint32_t)p->tx_buf;
    d.DMA_DIR                = DMA_DIR_MemoryToPeripheral;
    d.DMA_BufferSize         = 1;                       /* 占位，发送时再填实际长度 */
    d.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    d.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    d.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    d.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    d.DMA_Mode               = DMA_Mode_Normal;
    d.DMA_Priority           = DMA_Priority_Medium;
    d.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    DMA_Init(hw->tx_stream, &d);
}

/*
 * USART：8 位数据、1 位停止、无校验、无流控，收发都走 DMA，只开空闲中断
 *
 * USART_Init() 按 USART 所在总线（USART6 在 APB2 84MHz，USART3 在 APB1 42MHz）计算波特率分频。
 * 调用：serial_open()，此时接收 DMA 已经就绪，使能 USART 后收到的第一个字节就能搬走
 */
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

/*
 * 打开一个串口：引脚 -> 接收 DMA -> 发送 DMA -> USART（8N1）+ 空闲中断
 *
 * 信号量要在开中断之前建好（中断里会用）。重复调用只返回 true，不重新初始化。
 *
 * 参数：id       —— SERIAL_BD / SERIAL_TT
 *       baudrate —— 波特率，取自 app_config.h
 * 返回：true 成功；false 参数不对或建信号量失败
 * 调用：bd_task()、tt_task() 启动时
 */
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

/* ------------------------------ 收发 ------------------------------ */

/*
 * 从环形缓冲区取出读指针到写指针之间的数据（最多 size 字节），并移动读指针
 *
 * 写指针 = rx_size - DMA 剩余计数。数据可能跨过缓冲区末尾，最多分两段拷贝：
 *   读指针 < 写指针：[tail, head) 一段；
 *   读指针 > 写指针：先拷 [tail, 末尾)，再从开头拷 [0, head)。
 * 只有本任务改读指针、只有 DMA 改写指针，不需要关中断。
 * 任务超过约 1 秒不来读、DMA 绕一圈追上读指针时，旧数据会被覆盖；北斗语句有校验和，
 * 被破坏的语句会在解析时丢掉，不会被当成正确数据。
 *
 * 返回：取到的字节数
 * 调用：serial_read()
 */
static size_t rx_copy(const serial_hw_t *hw, serial_port_t *p, uint8_t *out, size_t size)
{
    size_t head = p->rx_size - DMA_GetCurrDataCounter(hw->rx_stream);
    size_t n    = 0;

    if (head >= p->rx_size) {               /* 计数器正好在重装的瞬间 */
        head = 0;
    }
    while (n < size && p->rx_tail != head) {
        size_t end   = (head > p->rx_tail) ? head : p->rx_size;     /* 这一段连续数据的末尾 */
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

/*
 * 取出已收到的数据；缓冲区是空的就等空闲中断，最多等 timeout_ms
 *
 * 先取一次，有数据直接返回；没有再等信号量，被唤醒或超时后再取一次。
 * 数据在两次之间到达时，中断已经给过信号量，Take 会立即返回，不会白等。
 *
 * 参数：id          —— 串口
 *       buf / size  —— 输出缓冲及大小
 *       timeout_ms  —— 没有数据时的最长等待时间，也是调用方主循环的最长周期
 * 返回：取到的字节数；超时或串口没打开返回 0
 * 调用：bd_task()、tt_task() 主循环，每圈一次
 */
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

/*
 * 用 DMA 发送一包数据：等上一包发完 -> 拷进发送缓冲 -> 清标志、填长度、使能数据流
 *
 * 发送时间 = 字节数 × 10 位 / 波特率，北斗 38400 下一条 4KB 的 TCQ 约 1.1 秒；
 * 函数不等它发完就返回，调用方的主循环照常运行、照常喂狗。
 * 重新使能数据流前必须清掉它的全部标志，否则启动不了；长度只能在数据流关闭时写。
 * RS422 / RS232 是全双工，不用等最后一个字节移出移位寄存器再切方向。
 *
 * 参数：id          —— 串口
 *       data / len  —— 待发数据，函数返回后调用方即可复用这块缓冲
 *       timeout_ms  —— 上一包还在发时最多等多久（调用方保证小于看门狗的 3 秒）
 * 返回：true 已启动发送；false 串口没打开、长度为 0 或超过发送缓冲、上一包超时没发完
 * 调用：bd_task.c 的 start_send()、query_card()
 */
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

    while (DMA_GetCmdStatus(hw->tx_stream) == ENABLE) {             /* 上一包还在发 */
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

/* ------------------------------ 中断 ------------------------------ */

/*
 * 串口中断公共处理：只处理空闲中断（线路上一串数据收完后空闲了 1 个字节的时间）
 *
 * 清空闲标志的方法是"先读状态寄存器 SR、再读数据寄存器 DR"（USART_GetITStatus() 读 SR，
 * USART_ReceiveData() 读 DR），这一步也会顺带清掉溢出错误标志。数据本身已经被 DMA 搬走，
 * 这里读到的值直接丢弃。然后释放信号量，唤醒在 serial_read() 里等数据的任务。
 *
 * 参数：id —— 串口
 * 调用：USART6_IRQHandler()、USART3_IRQHandler()
 */
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

/* 北斗串口 USART6 中断（启动文件向量表里的名字） */
void USART6_IRQHandler(void)
{
    serial_irq(SERIAL_BD);
}

/* 天通串口 USART3 中断 */
void USART3_IRQHandler(void)
{
    serial_irq(SERIAL_TT);
}
