/*
 * STM32F103 backup-domain RTC. The counter is an arbitrary-epoch seconds
 * value; callers only use its time-of-day component.
 */
#ifdef CONFIG_BOARD_STM32F103

#include "rtc.h"
#include "stm32_uart.h"

#define RTC_SECONDS_PER_DAY 86400U

uint32_t rtc_hms_to_secs(int hour, int minute, int second) {
    return (uint32_t)(hour * 3600 + minute * 60 + second);
}

void rtc_secs_to_hms(uint32_t seconds, int *hour, int *minute, int *second) {
    seconds %= RTC_SECONDS_PER_DAY;
    if (hour)
        *hour = (int)(seconds / 3600U);
    seconds %= 3600U;
    if (minute)
        *minute = (int)(seconds / 60U);
    if (second)
        *second = (int)(seconds % 60U);
}

#ifdef CONFIG_MCU

#define RCC_APB1ENR (*(volatile uint32_t *)0x4002101CUL)
#define RCC_BDCR    (*(volatile uint32_t *)0x40021020UL)
#define RCC_CSR     (*(volatile uint32_t *)0x40021024UL)
#define PWR_CR      (*(volatile uint32_t *)0x40007000UL)

#define RTC_CRH     (*(volatile uint32_t *)0x40002800UL)
#define RTC_CRL     (*(volatile uint32_t *)0x40002804UL)
#define RTC_PRLH    (*(volatile uint32_t *)0x40002808UL)
#define RTC_PRLL    (*(volatile uint32_t *)0x4000280CUL)
#define RTC_CNTH    (*(volatile uint32_t *)0x40002818UL)
#define RTC_CNTL    (*(volatile uint32_t *)0x4000281CUL)

#define BKP_DR1     (*(volatile uint32_t *)0x40006C04UL)
#define CORE_DEMCR  (*(volatile uint32_t *)0xE000EDFCUL)
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004UL)

#define RCC_APB1ENR_PWREN (1U << 28)
#define RCC_APB1ENR_BKPEN (1U << 27)
#define PWR_CR_DBP         (1U << 8)
#define RCC_BDCR_LSEON     (1U << 0)
#define RCC_BDCR_LSERDY    (1U << 1)
#define RCC_BDCR_RTCSEL    (3U << 8)
#define RCC_BDCR_RTCSEL_LSE (1U << 8)
#define RCC_BDCR_RTCSEL_LSI (2U << 8)
#define RCC_BDCR_RTCEN     (1U << 15)
#define RCC_BDCR_BDRST     (1U << 16)
#define RCC_CSR_LSION      (1U << 0)
#define RCC_CSR_LSIRDY     (1U << 1)
#define RTC_CRL_RTOFF      (1U << 5)
#define RTC_CRL_CNF        (1U << 4)
#define RTC_CRL_RSF        (1U << 3)

#define RTC_BKP_MAGIC      0xA201U
#define CORE_DEMCR_TRCENA  (1U << 24)
#define DWT_CTRL_CYCCNTENA (1U << 0)
#define RTC_SYNC_WAIT_LOOPS 100000U

static int rtc_running;

static int rtc_wait(uint32_t mask) {
    uint32_t i;

    for (i = 0; i < RTC_SYNC_WAIT_LOOPS; i++) {
        if ((RTC_CRL & mask) == mask)
            return 1;
    }
    return 0;
}

static int rcc_wait_ms(volatile uint32_t *reg, uint32_t mask,
                       uint32_t timeout_ms, uint32_t max_loops) {
    uint32_t hclk = stm32_hclk_hz();
    uint32_t start;
    uint32_t timeout_cycles;

    CORE_DEMCR |= CORE_DEMCR_TRCENA;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    start = DWT_CYCCNT;
    timeout_cycles = (hclk / 1000U) * timeout_ms;
    for (uint32_t loops = 0; loops < max_loops; loops++) {
        if (*reg & mask)
            return 1;
        if ((uint32_t)(DWT_CYCCNT - start) >= timeout_cycles)
            break;
    }
    return 0;
}

static void rtc_backup_reset(void) {
    RCC_BDCR |= RCC_BDCR_BDRST;
    RCC_BDCR &= ~RCC_BDCR_BDRST;
}

/* RM0008 requires synchronizing the APB1-visible RTC registers after RTCSEL
 * or RTCEN changes. No counter/prescaler access is valid until RSF returns. */
static int rtc_sync(void) {
    RTC_CRL &= ~RTC_CRL_RSF;
    return rtc_wait(RTC_CRL_RSF);
}

static int rtc_enter_config(void) {
    if (!rtc_wait(RTC_CRL_RTOFF))
        return 0;
    RTC_CRL |= RTC_CRL_CNF;
    return 1;
}

static int rtc_leave_config(void) {
    RTC_CRL &= ~RTC_CRL_CNF;
    return rtc_wait(RTC_CRL_RTOFF);
}

static uint32_t rtc_counter(void) {
    uint32_t high1;
    uint32_t high2;
    uint32_t low;

    do {
        high1 = RTC_CNTH & 0xFFFFU;
        low = RTC_CNTL & 0xFFFFU;
        high2 = RTC_CNTH & 0xFFFFU;
    } while (high1 != high2);
    return (high1 << 16) | low;
}

static int rtc_write_counter(uint32_t seconds) {
    if (!rtc_enter_config())
        return 0;
    RTC_CNTH = seconds >> 16;
    RTC_CNTL = seconds & 0xFFFFU;
    return rtc_leave_config();
}

static int rtc_select_clock(uint32_t source, uint32_t prescaler) {
    RCC_BDCR = (RCC_BDCR & ~RCC_BDCR_RTCSEL) | source | RCC_BDCR_RTCEN;
    if (!rtc_sync() || !rtc_enter_config())
        return 0;
    RTC_PRLH = 0;
    RTC_PRLL = prescaler;
    return rtc_leave_config();
}

static int rtc_source_valid(void) {
    uint32_t source = RCC_BDCR & RCC_BDCR_RTCSEL;

    return (source == RCC_BDCR_RTCSEL_LSE || source == RCC_BDCR_RTCSEL_LSI) &&
           (RCC_BDCR & RCC_BDCR_RTCEN) != 0U;
}

void stm32_rtc_init(void) {
    RCC_APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR_CR |= PWR_CR_DBP;
    rtc_running = 0;

    /* A configured backup domain keeps counting through reset; only reset it
     * after its marker/source validation or register synchronization fails. */
    if ((BKP_DR1 & 0xFFFFU) == RTC_BKP_MAGIC && rtc_source_valid() &&
        rtc_sync()) {
        rtc_running = 1;
        return;
    }

    /* No usable persisted clock: reset stale source selection before choosing
     * LSE, then retry from a clean backup domain with LSI on any LSE failure. */
    rtc_backup_reset();
    RCC_BDCR |= RCC_BDCR_LSEON;
    if (rcc_wait_ms(&RCC_BDCR, RCC_BDCR_LSERDY, 2000U, 20000000U) &&
        rtc_select_clock(RCC_BDCR_RTCSEL_LSE, 32767U)) {
        /* LSE selected and synchronized. */
    } else {
        rtc_backup_reset();
        RCC_BDCR &= ~RCC_BDCR_LSEON;
        RCC_CSR |= RCC_CSR_LSION;
        if (!rcc_wait_ms(&RCC_CSR, RCC_CSR_LSIRDY, 100U, 1000000U) ||
            !rtc_select_clock(RCC_BDCR_RTCSEL_LSI, 39999U))
            return;
    }

    if (!rtc_write_counter(rtc_hms_to_secs(12, 0, 0)))
        return;
    BKP_DR1 = RTC_BKP_MAGIC;
    rtc_running = 1;
}

void stm32_rtc_get_hhmmss(int *hour, int *minute, int *second) {
    if (!rtc_running) {
        if (hour)
            *hour = 12;
        if (minute)
            *minute = 0;
        if (second)
            *second = 0;
        return;
    }
    rtc_secs_to_hms(rtc_counter(), hour, minute, second);
}

int stm32_rtc_set_hhmmss(int hour, int minute, int second) {
    if (hour < 0 || hour >= 24 || minute < 0 || minute >= 60 || second < 0 ||
        second >= 60 || !rtc_running)
        return -1;
    if (!rtc_write_counter(rtc_hms_to_secs(hour, minute, second)))
        return -1;
    BKP_DR1 = RTC_BKP_MAGIC;
    return 0;
}

int stm32_rtc_available(void) { return rtc_running; }

#endif /* CONFIG_MCU */

#endif /* CONFIG_BOARD_STM32F103 */
