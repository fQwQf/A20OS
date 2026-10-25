/* STM32F103 UID + committed two-page boot epoch. Hardware facts: ST PM0075
 * sections 2.3/3, and ST's CMSIS stm32f103xb.h UID/FLASH definitions.
 * Called once, with interrupts disabled, before the leaf can receive frames. */
#include "board.h"
#include "board_config.h"
#include "mcu/boot_epoch.h"

#ifndef CONFIG_STM32_QEMU
#define FLASH_KEYR (*(volatile uint32_t *)(STM32_FLASH_CONTROL_BASE + 4u))
#define FLASH_SR (*(volatile uint32_t *)(STM32_FLASH_CONTROL_BASE + 12u))
#define FLASH_CR (*(volatile uint32_t *)(STM32_FLASH_CONTROL_BASE + 16u))
#define FLASH_AR (*(volatile uint32_t *)(STM32_FLASH_CONTROL_BASE + 20u))
#define FLASH_BUSY 1u
#define FLASH_ERRORS ((1u << 2) | (1u << 4))
#define FLASH_DONE (1u << 5)
#define FLASH_PROGRAM 1u
#define FLASH_PAGE_ERASE (1u << 1)
#define FLASH_START (1u << 6)
#define FLASH_LOCK (1u << 7)
#define RAM_CODE __attribute__((section(".ramfunc"), noinline))
extern uint8_t _cluster_nv_start[], _cluster_nv_end[];
typedef struct flash_context {
    uintptr_t base;
    uint32_t page_bytes;
} flash_context_t;

static RAM_CODE int flash_wait(void) {
    for (unsigned i = 0; i < 1000000; i++)
        if (!(FLASH_SR & FLASH_BUSY))
            return (FLASH_SR & FLASH_ERRORS) ? -1 : 0;
    return -1;
}
static uint16_t flash_read16(void *ctx, uint32_t offset) {
    flash_context_t *f = ctx;
    return *(volatile uint16_t *)(f->base + offset);
}
static RAM_CODE int flash_program16(void *ctx, uint32_t offset,
                                    uint16_t value) {
    flash_context_t *f = ctx;
    if ((offset & 1) || offset >= 2 * f->page_bytes || flash_wait() < 0)
        return -1;
    volatile uint16_t *word = (void *)(f->base + offset);
    if (*word != 0xffffu)
        return -1;
    if (value == 0xffffu)
        return 0;
    FLASH_SR = FLASH_ERRORS | FLASH_DONE;
    FLASH_CR = FLASH_PROGRAM;
    *word = value;
    int r = flash_wait();
    FLASH_CR = 0;
    return r < 0 || *word != value ? -1 : 0;
}
static RAM_CODE int flash_erase_page(void *ctx, uint32_t page) {
    flash_context_t *f = ctx;
    if (page > 1 || flash_wait() < 0)
        return -1;
    FLASH_SR = FLASH_ERRORS | FLASH_DONE;
    FLASH_CR = FLASH_PAGE_ERASE;
    FLASH_AR = (uint32_t)(f->base + page * f->page_bytes);
    FLASH_CR = FLASH_PAGE_ERASE | FLASH_START;
    int r = flash_wait();
    FLASH_CR = 0;
    return r;
}
#endif

int stm32_cluster_boot_identity(uint8_t node_id[16], uint64_t *epoch) {
#ifdef CONFIG_STM32_QEMU
    (void)node_id;
    (void)epoch;
    return -1;
#else
    if (!node_id || !epoch)
        return -1;
    uint32_t kb = *(volatile uint16_t *)0x1ffff7e0u;
    uint32_t page_bytes = kb > 128 ? 2048 : 1024;
    uintptr_t base = (uintptr_t)_cluster_nv_start;
    if (kb != STM32_FLASH_KB ||
        (uintptr_t)_cluster_nv_end - base != 2 * page_bytes ||
        base % page_bytes)
        return -1;
    const volatile uint8_t *uid = (void *)0x1ffff7e8u;
    unsigned any = 0, all = 255;
    node_id[0] = 'A';
    node_id[1] = '2';
    node_id[2] = '0';
    node_id[3] = 'M';
    for (unsigned i = 0; i < 12; i++) {
        uint8_t byte = uid[i];
        any |= byte;
        all &= byte;
        node_id[4 + i] = byte;
    }
    if (!any || all == 255)
        return -1;
    if (FLASH_SR & FLASH_BUSY)
        return -1;
    FLASH_SR = FLASH_ERRORS | FLASH_DONE;
    if (FLASH_CR & FLASH_LOCK) {
        FLASH_KEYR = 0x45670123u;
        FLASH_KEYR = 0xcdef89abu;
    }
    if (FLASH_CR & FLASH_LOCK)
        return -1;
    flash_context_t context = {base, page_bytes};
    const a20_epoch_store_t store = {&context, page_bytes, flash_read16,
                                     flash_program16, flash_erase_page};
    int result = a20_epoch_next(&store, epoch);
    FLASH_CR = FLASH_LOCK;
    return result;
#endif
}
