/*
 * uuid.c —— RFC 4122 第 4 版 UUID
 *
 * 随机源：
 *   首选 RNG（真随机数发生器，时钟来自 PLL 的 48MHz 输出 PLLQ：8MHz / 8 × 336 / 7 = 48MHz，
 *   system_stm32f4xx.c 的默认配置即可工作），每读一次数据寄存器得到 32 位真随机数；
 *   RNG 初始化失败或运行中出错（时钟错误 / 种子错误）时，改用 xorshift32 伪随机数，
 *   种子 = 芯片 96 位唯一 ID ^ 当前 tick ^ SysTick 计数值，不同设备、不同开机时刻的序列都不同。
 *
 * 调用关系：uuid_generate() -> rand32() -> rng_read() / prng_next()
 */
#include <stdbool.h>
#include <stdio.h>
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"
#include "uuid.h"

#define RNG_WAIT_LOOPS      10000U      /* 等一个随机数就绪的最大查询次数（正常几十次就好） */
#define CHIP_UID_ADDR       0x1FFF7A10U /* 芯片 96 位唯一 ID，连续 3 个字 */
#define CHIP_UID(n)         (*(const volatile uint32_t *)(CHIP_UID_ADDR + 4U * (n)))

static bool     s_rng_ok;               /* RNG 可用；出错后置 false，之后一直用伪随机 */
static uint32_t s_seed;                 /* 伪随机数状态 */

/*
 * 等 RNG 出一个新的 32 位随机数
 *
 * 数据就绪（DRDY）就读出；出现时钟错误或种子错误、或等太久，返回 false，由调用方改用伪随机。
 *
 * 参数：out —— 输出的随机数
 * 返回：true 成功
 * 调用：uuid_init()、rand32()
 */
static bool rng_read(uint32_t *out)
{
    uint32_t i;

    for (i = 0; i < RNG_WAIT_LOOPS; i++) {
        if (SET == RNG_GetFlagStatus(RNG_FLAG_CECS) || SET == RNG_GetFlagStatus(RNG_FLAG_SECS)) {
            return false;
        }
        if (SET == RNG_GetFlagStatus(RNG_FLAG_DRDY)) {
            *out = RNG_GetRandomNumber();
            return true;
        }
    }
    return false;
}

/*
 * xorshift32 伪随机数：只在 RNG 不可用时使用
 *
 * 每次都混入当前 tick 和 SysTick 计数值，即使种子相同，生成时刻不同结果也不同。
 * 调用：rand32()
 */
static uint32_t prng_next(void)
{
    s_seed ^= xTaskGetTickCount() ^ SysTick->VAL;
    if (s_seed == 0U) {
        s_seed = 0x9E3779B9U;               /* xorshift 的状态不能是 0 */
    }
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

/*
 * 取一个 32 位随机数：RNG 优先，失败就永久切换到伪随机
 *
 * 调用：uuid_generate()，在临界区里
 */
static uint32_t rand32(void)
{
    uint32_t r;

    if (s_rng_ok) {
        if (rng_read(&r)) {
            return r;
        }
        s_rng_ok = false;
    }
    return prng_next();
}

/*
 * 打开 RNG 并试读一次，确认它能工作；同时准备好伪随机数的种子
 *
 * 调用：main()，调度器启动前（这时还不能用临界区，也没有别的任务在用 RNG）
 */
void uuid_init(void)
{
    uint32_t r;

    s_seed = CHIP_UID(0) ^ CHIP_UID(1) ^ CHIP_UID(2);

    RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_RNG, ENABLE);
    RNG_DeInit();
    RNG_Cmd(ENABLE);
    s_rng_ok = rng_read(&r);
    if (s_rng_ok) {
        s_seed ^= r;
    }
    LOG("UUID: random source %s", s_rng_ok ? "RNG" : "pseudo (RNG not ready)");
}

/*
 * 生成一个 UUID 字符串：16 个随机字节 -> 设置版本位和变体位 -> 格式化成 8-4-4-4-12
 *
 *   字节 6 的高 4 位置 0100：版本 4（随机型）
 *   字节 8 的高 2 位置 10  ：RFC 4122 变体
 * 取随机数时进临界区（4 次读 RNG 只要几微秒），几个任务同时调用也不会拿到同一个数；
 * 格式化在临界区外做。
 *
 * 参数：out —— 至少 UUID_STR_SIZE（37）字节，结果以 '\0' 结尾
 * 调用：term_task.c 的 handle_json()（请求里缺 msg_id / taskid 时补）、handle_device_msg()（主动上报）
 */
void uuid_generate(char out[UUID_STR_SIZE])
{
    uint8_t b[16];
    int     i;

    taskENTER_CRITICAL();
    for (i = 0; i < 16; i += 4) {
        uint32_t r = rand32();

        b[i]     = (uint8_t)r;
        b[i + 1] = (uint8_t)(r >> 8);
        b[i + 2] = (uint8_t)(r >> 16);
        b[i + 3] = (uint8_t)(r >> 24);
    }
    taskEXIT_CRITICAL();

    b[6] = (uint8_t)((b[6] & 0x0FU) | 0x40U);       /* 版本 4 */
    b[8] = (uint8_t)((b[8] & 0x3FU) | 0x80U);       /* 变体 10xx */

    snprintf(out, UUID_STR_SIZE,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
