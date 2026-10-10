#include "core/types.h"
#include "core/string.h"
#include "core/consts.h"
#include "drivers/core/driver_hwapi.h"

/* ---- Whitelisted user-mappable MMIO windows ---- */

typedef struct {
    uint64_t base;
    uint64_t size;
    const char *name;
    uint8_t user_owned; /* kernel probe must skip this device */
} udriver_mmio_window_t;

static const udriver_mmio_window_t g_mmio_windows[] = {
    {0x101000, 0x1000, "goldfish-rtc", 1},
    /* virtio-mmio slot 3: reserved for the user-space virtio-blk pilot
     * (docs/hybrid-kernel/02-mainstream-plan.md M4).  The kernel's
     * virtio_mmio_enumerate() skips user-owned slots. */
    {0x10004000, 0x1000, "virtio-blk-user", 1},
    /* virtio-mmio slot 5: dual-placement virtio-input sample
     * (docs/hybrid-kernel/04-dual-placement.md).  User-owned so the
     * in-tree kernel driver never binds; the kernel placement probe
     * (the vinput-probe module) is read-only and does not bind either. */
    {0x10006000, 0x1000, "virtio-input-dual", 1},
};

#define UDRIVER_MMIO_WINDOWS_NR                                                \
    ((int)(sizeof(g_mmio_windows) / sizeof(g_mmio_windows[0])))

int board_user_mmio_allowed(uint64_t phys, uint64_t size) {
    if (!size || phys + size < phys)
        return 0;
    for (int i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++) {
        uint64_t wb = g_mmio_windows[i].base;
        if (phys >= wb && phys + size <= wb + g_mmio_windows[i].size)
            return 1;
    }
    return 0;
}

/* The GUI QEMU profile has a virtio-gpu at MMIO slot 7 and uses both
 * virtio-input devices through the kernel evdev mux.  The text/dual-input
 * profile has no GPU and keeps slot 5 reserved for uinputd. */
static int udriver_gui_kernel_input(void) {
    uintptr_t base = PAGE_OFFSET + 0x10008000ULL;
    return readl((const volatile void *)base) == 0x74726976U &&
           readl((const volatile void *)(base + 0x008)) == 16U;
}

/* Board device enumeration consults this before binding a kernel driver
 * to a device that belongs to a user-space driver. */
int board_user_mmio_owned(uint64_t phys) {
    for (int i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++)
        if (g_mmio_windows[i].user_owned && phys >= g_mmio_windows[i].base &&
            phys < g_mmio_windows[i].base + g_mmio_windows[i].size) {
            if (phys == 0x10006000ULL && udriver_gui_kernel_input())
                return 0;
            return 1;
        }
    return 0;
}

/* Hardware presence probe for a user-claimable window.  virtio-mmio slots
 * are identified by their magic register + device id; declared board
 * devices (goldfish RTC) are always present on their board. */
int board_user_mmio_present(uint64_t phys) {
    for (int i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++) {
        if (phys >= g_mmio_windows[i].base &&
            phys < g_mmio_windows[i].base + g_mmio_windows[i].size) {
            if (strncmp(g_mmio_windows[i].name, "virtio-", 7) == 0) {
                if (phys == 0x10006000ULL && udriver_gui_kernel_input())
                    return 0;
                uintptr_t base = PAGE_OFFSET + g_mmio_windows[i].base;
                uint32_t magic = readl((const volatile void *)base);
                uint32_t dev_id = readl((const volatile void *)(base + 0x008));
                return magic == 0x74726976U && dev_id != 0;
            }
            return 1; /* declared board device (goldfish-rtc) */
        }
    }
    return 0;
}

/* Claim slots are local policy identifiers; the last slot is reserved for PCI. */
int board_user_mmio_index(uint64_t phys) {
    for (int i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++)
        if (g_mmio_windows[i].user_owned && phys >= g_mmio_windows[i].base &&
            phys < g_mmio_windows[i].base + g_mmio_windows[i].size)
            return i;
    return -1;
}
