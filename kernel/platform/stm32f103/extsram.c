#ifdef CONFIG_BOARD_STM32F103

#include "extsram.h"
#include "core/arch.h"
#include "core/string.h"

#define RCC_AHBENR  (*(volatile uint32_t *)0x40021014UL)
#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOD_CRL (*(volatile uint32_t *)0x40011400UL)
#define GPIOD_CRH (*(volatile uint32_t *)0x40011404UL)
#define GPIOE_CRL (*(volatile uint32_t *)0x40011800UL)
#define GPIOE_CRH (*(volatile uint32_t *)0x40011804UL)
#define GPIOF_CRL (*(volatile uint32_t *)0x40011C00UL)
#define GPIOF_CRH (*(volatile uint32_t *)0x40011C04UL)
#define GPIOG_CRL (*(volatile uint32_t *)0x40012000UL)
#define GPIOG_CRH (*(volatile uint32_t *)0x40012004UL)

#define FSMC_BCR3  (*(volatile uint32_t *)0xA0000010UL)
#define FSMC_BTR3  (*(volatile uint32_t *)0xA0000014UL)
#define FSMC_BWTR3 (*(volatile uint32_t *)0xA0000110UL)

typedef struct ext_block {
    size_t size;
    struct ext_block *next;
    uint32_t free;
} ext_block_t;

static ext_block_t *ext_head;
static int ext_ready;

static size_t align8(size_t value) {
    return (value + 7U) & ~(size_t)7U;
}

static void gpio_config_pin(volatile uint32_t *crl, volatile uint32_t *crh,
                            unsigned pin, uint32_t mode) {
    volatile uint32_t *reg = pin < 8U ? crl : crh;
    uint32_t shift = (pin & 7U) * 4U;
    uint32_t value = *reg;
    value &= ~(0xFU << shift);
    value |= mode << shift;
    *reg = value;
}

static int extsram_probe(void) {
    volatile uint32_t *const base =
        (volatile uint32_t *)(uintptr_t)STM32_EXTSRAM_BASE;
    static const uint32_t offsets[] = {
        0U, 4U, 0x100U, 0x1000U, STM32_EXTSRAM_SIZE / 2U,
        STM32_EXTSRAM_SIZE - 4U,
    };
    static const uint32_t patterns[] = {
        0x55AA33CCU, 0xAA55CC33U, 0x01234567U, 0x89ABCDEFU,
        0xA20F103EU, 0x5A5AC3C3U,
    };
    uint32_t saved[sizeof(offsets) / sizeof(offsets[0])];

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint32_t *p =
            (volatile uint32_t *)((uintptr_t)base + offsets[i]);
        saved[i] = *p;
        *p = patterns[i];
    }
    __asm__ __volatile__("dsb" ::: "memory");

    int ok = 1;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint32_t *p =
            (volatile uint32_t *)((uintptr_t)base + offsets[i]);
        if (*p != patterns[i])
            ok = 0;
    }

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint32_t *p =
            (volatile uint32_t *)((uintptr_t)base + offsets[i]);
        *p = saved[i];
    }
    return ok;
}

int stm32_extsram_init(void) {
    ext_ready = 0;
    ext_head = NULL;

#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    RCC_APB2ENR |= (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8);
    RCC_AHBENR |= 1U << 8;

    const unsigned pd[] = {0, 1, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
    const unsigned pe[] = {0, 1, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const unsigned pf[] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15};
    const unsigned pg[] = {0, 1, 2, 3, 4, 5, 10};

    for (unsigned i = 0; i < sizeof(pd) / sizeof(pd[0]); i++)
        gpio_config_pin(&GPIOD_CRL, &GPIOD_CRH, pd[i], 0xBU);
    for (unsigned i = 0; i < sizeof(pe) / sizeof(pe[0]); i++)
        gpio_config_pin(&GPIOE_CRL, &GPIOE_CRH, pe[i], 0xBU);
    for (unsigned i = 0; i < sizeof(pf) / sizeof(pf[0]); i++)
        gpio_config_pin(&GPIOF_CRL, &GPIOF_CRH, pf[i], 0xBU);
    for (unsigned i = 0; i < sizeof(pg) / sizeof(pg[0]); i++)
        gpio_config_pin(&GPIOG_CRL, &GPIOG_CRH, pg[i], 0xBU);

    /* IS62WV51216: asynchronous 16-bit SRAM on FSMC Bank1 NOR/SRAM3. */
    FSMC_BCR3 = (1U << 14) | (1U << 12) | (1U << 4) | (1U << 0);
    FSMC_BTR3 = 1U | (5U << 8);
    FSMC_BWTR3 = 1U | (3U << 8);

    if (!extsram_probe()) {
        FSMC_BCR3 &= ~1U;
        return -1;
    }

    ext_head = (ext_block_t *)(uintptr_t)STM32_EXTSRAM_BASE;
    ext_head->size = STM32_EXTSRAM_SIZE - sizeof(*ext_head);
    ext_head->next = NULL;
    ext_head->free = 1;
    ext_ready = 1;
    return 0;
#endif
}

void stm32_extsram_shutdown(void) {
    ext_ready = 0;
    ext_head = NULL;
#ifdef CONFIG_STM32_XUANWU
    FSMC_BCR3 &= ~1U;
#endif
}

int stm32_extsram_ready(void) {
    return ext_ready;
}

size_t stm32_extsram_available(void) {
    size_t total = 0;
    uint32_t flags = arch_irq_save();
    for (ext_block_t *b = ext_head; b; b = b->next)
        if (b->free)
            total += b->size;
    arch_irq_restore(flags);
    return total;
}

void *stm32_extsram_alloc(size_t size) {
    if (!ext_ready || !size)
        return NULL;
    size = align8(size);
    uint32_t flags = arch_irq_save();
    for (ext_block_t *b = ext_head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;
        if (b->size >= size + sizeof(*b) + 8U) {
            ext_block_t *tail =
                (ext_block_t *)((char *)(b + 1) + size);
            tail->size = b->size - size - sizeof(*tail);
            tail->next = b->next;
            tail->free = 1;
            b->next = tail;
            b->size = size;
        }
        b->free = 0;
        arch_irq_restore(flags);
        return b + 1;
    }
    arch_irq_restore(flags);
    return NULL;
}

void stm32_extsram_free(void *ptr) {
    if (!stm32_extsram_owns(ptr))
        return;

    uint32_t flags = arch_irq_save();
    ext_block_t *block = (ext_block_t *)ptr - 1;
    block->free = 1;
    for (ext_block_t *b = ext_head; b && b->next;) {
        if (b->free && b->next->free) {
            b->size += sizeof(*b) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
    arch_irq_restore(flags);
}

int stm32_extsram_owns(const void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
    return ext_ready &&
           value >= STM32_EXTSRAM_BASE + sizeof(ext_block_t) &&
           value < STM32_EXTSRAM_BASE + STM32_EXTSRAM_SIZE;
}

size_t stm32_extsram_allocation_size(const void *ptr) {
    if (!stm32_extsram_owns(ptr))
        return 0;
    const ext_block_t *block = (const ext_block_t *)ptr - 1;
    return block->free ? 0 : block->size;
}

#endif
