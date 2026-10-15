#include <stdbool.h>
#include <stdio.h>
#include "mcu.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"
#include "uuid.h"

#define RNG_WAIT_LOOPS      10000U
#define CHIP_UID_ADDR       0x1FFF7A10U
#define CHIP_UID(n)         (*(const volatile uint32_t *)(CHIP_UID_ADDR + 4U * (n)))

static bool     s_rng_ok;
static uint32_t s_seed;

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

static uint32_t prng_next(void)
{
    s_seed ^= xTaskGetTickCount() ^ SysTick->VAL;
    if (s_seed == 0U) {
        s_seed = 0x9E3779B9U;
    }
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

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

    b[6] = (uint8_t)((b[6] & 0x0FU) | 0x40U);
    b[8] = (uint8_t)((b[8] & 0x3FU) | 0x80U);

    snprintf(out, UUID_STR_SIZE,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
