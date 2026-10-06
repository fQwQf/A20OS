/*
 * DW-SDIO (Synopsys DesignWare SDIO/MMC Host Controller) Driver
 *
 * Supports VisionFive2 and other boards with DW-SDIO.
 * Reference: rocketos os/src/drivers/block/sdio.rs
 */
#ifndef _DW_SDIO_H
#define _DW_SDIO_H

#include "core/types.h"
#include "core/string.h"

/* Public API for FS layer */
#define DW_SDIO_SECTOR_SIZE 512

/* Platform identity contract: the board registers its SDIO controller as a
 * platform device with these IDs; the driver binds through platform_bus. */
#define DW_SDIO_PLATFORM_VENDOR 0x5F56U
#define DW_SDIO_PLATFORM_DEVICE 1U

/*
 * a20.dw-sdio.poll=1 keeps the completion path in bounded polling even when the
 * platform device published a usable IRQ line.  This is a kernel parameter
 * rather than a build flag on purpose: whether a board's line is actually
 * routed is a property of that board, one image has to boot on all of them,
 * and a fallback nothing can select is a fallback nobody can test.
 */
#define DW_SDIO_POLL_KEY "a20.dw-sdio.poll="

/*
 * Completion-mode decision, kept in the header and inline so the driver and the
 * CONFIG_PLATFORM_IRQ_TEST self-check reach the same rule without the check
 * having to link the block driver.
 *
 * @have_irq_line is platform_device_irq()'s return: >= 0 for a real line, a
 * negative errno for a device that published none (or a broken one).
 * @force_poll is the a20.dw-sdio.poll=1 read.
 *
 * Returns 1 for "drive the completion path from the interrupt", 0 for "poll".
 * A line is only claimed when the board actually published one, so a device
 * that never had a RES_IRQ behaves exactly as it did before this existed.
 */
static inline int dw_sdio_pick_irq_mode(int have_irq_line, int force_poll)
{
    return (have_irq_line >= 0 && !force_poll) ? 1 : 0;
}

/*
 * Scan a kernel command line for a20.dw-sdio.poll=1.  Split out so the
 * self-check can feed it a synthetic string: the parser the driver runs at
 * probe time is the parser under test, not a copy of it.
 */
static inline int dw_sdio_poll_forced(const char *cmdline)
{
    const char *p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        size_t klen = sizeof(DW_SDIO_POLL_KEY) - 1;
        if ((size_t)(p - tok) > klen &&
            memcmp(tok, DW_SDIO_POLL_KEY, klen) == 0 &&
            tok[klen] >= '1' && tok[klen] <= '9')
            return 1;
    }
    return 0;
}

int  dw_sdio_init_dev(uintptr_t base);
int  dw_sdio_read_sector(uintptr_t base, uint64_t lba, void *buf, size_t count);
int  dw_sdio_write_sector(uintptr_t base, uint64_t lba, const void *buf, size_t count);
uint64_t dw_sdio_capacity(uintptr_t base);
int  dw_sdio_card_ready(uintptr_t base);

/*
 * Which completion path the instance is actually running: 1 = interrupt
 * driven, 0 = polling.  Also 0 after a completion timeout downgraded the
 * instance, so a caller reporting the active mode does not report the
 * requested one.
 */
int  dw_sdio_using_irq(void);

#endif /* _DW_SDIO_H */
