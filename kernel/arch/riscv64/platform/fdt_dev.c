/*
 * Device-tree device enumeration for RISC-V boards.
 *
 * A board file used to spell out every device it has: the MMIO base, the size
 * and the IRQ, hand-copied out of a device tree into #defines, and then repeated
 * as a platform_device_t with a vendor number that had to agree with the
 * driver's.  Both halves are things the firmware already told us, so this reads
 * them instead.
 *
 * The walker collects each node's properties and, when the node's `compatible`
 * matches a driver that registered an of_compatible string, builds the
 * platform device from the `reg` and `interrupts` the tree carries.  A node that
 * no driver claims, or that the tree marks `status = "disabled"`, is skipped --
 * which is how the JH7110 tree keeps a second GMAC from being enumerated when
 * the board only wired one of them out.
 *
 * The lookup goes through the driver model rather than a table here, so this
 * file never names a driver or a vendor number.  Adding a board with a new
 * SoC is a matter of writing the driver with its compatible string; no board
 * file has to learn the driver's identity.
 */
#ifdef CONFIG_RISCV64

#include "core/types.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/errno.h"
#include "platform.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"

/* Saved from a1 by the boot stub: the tree the firmware booted with. */
extern uint64_t __boot_dtb_ptr;

#define FDT_MAGIC        0xd00dfeedU
#define FDT_BEGIN_NODE   1U
#define FDT_END_NODE     2U
#define FDT_PROP         3U
#define FDT_NOP          4U
#define FDT_END          9U

/* A node needs at most an MMIO region and an IRQ.  Sixteen covers every SoC
 * this kernel currently boots; a board that needs more should say so here. */
#define DT_MAX_DEVICES 16

struct dt_slot {
    platform_device_t pdev;
    resource_t         res[2];
    char               name[48];
};

static struct dt_slot g_dt_slots[DT_MAX_DEVICES];
static int            g_dt_slot_count;

static uint32_t dt_be32(uint32_t v)
{
    return ((v >> 24) & 0xffU) | ((v >> 8) & 0xff00U) |
           ((v << 8) & 0xff0000U) | ((v << 24) & 0xff000000U);
}

static uint32_t dt_read32(const void *p)
{
    return dt_be32(*(const uint32_t *)p);
}

/* Cell counts default to the RISC-V 2/2 that every SoC tree here uses; the
 * root node's own #address-cells/#size-cells override them when present. */
#define DT_DEFAULT_ADDRESS_CELLS 2
#define DT_DEFAULT_SIZE_CELLS     2

static uint64_t dt_cells_to_u64(const uint32_t *cells, uint32_t n)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++)
        v = (v << 32) | cells[i];
    return v;
}

/* `compatible` is a list of NUL-separated strings, most specific first.  Try
 * each until a driver claims one. */
static int dt_claim_compatible(const char *value, uint32_t len,
                               uint32_t *vendor, uint32_t *device)
{
    uint32_t i = 0;
    while (i < len && value[i]) {
        uint32_t start = i;
        while (i < len && value[i])
            i++;
        if (driver_lookup_compatible(value + start, vendor, device) == 0)
            return 1;
        i++;
    }
    return 0;
}

static int dt_slot_emit(const char *node_name, uint32_t name_len,
                        uint64_t mmio_base, uint64_t mmio_size,
                        uint32_t irq, int has_irq,
                        uint32_t vendor, uint32_t device)
{
    if (g_dt_slot_count >= DT_MAX_DEVICES) {
        kwarn("[DT] device pool exhausted, dropping \"%s\"\n", node_name);
        return -ENOMEM;
    }

    struct dt_slot *slot = &g_dt_slots[g_dt_slot_count];
    uint32_t copy = name_len;
    if (copy >= sizeof(slot->name))
        copy = sizeof(slot->name) - 1;
    memcpy(slot->name, node_name, copy);
    slot->name[copy] = '\0';

    int res_count = 0;
    if (mmio_size) {
        slot->res[res_count].type  = RES_MMIO;
        slot->res[res_count].start = mmio_base + PAGE_OFFSET;
        slot->res[res_count].end   = mmio_base + mmio_size - 1 + PAGE_OFFSET;
        slot->res[res_count].flags = IORESOURCE_MMIO_32BIT;
        res_count++;
    }
    if (has_irq) {
        slot->res[res_count].type  = RES_IRQ;
        slot->res[res_count].start = irq;
        slot->res[res_count].end   = irq;
        slot->res[res_count].flags = 0;
        res_count++;
    }
    if (!res_count) {
        kwarn("[DT] \"%s\" has no reg or interrupts, skipping\n", slot->name);
        return -EINVAL;
    }

    slot->pdev.dev.name      = slot->name;
    slot->pdev.dev.res       = slot->res;
    slot->pdev.dev.res_count = res_count;
    slot->pdev.dev.state     = DEV_STATE_UNINIT;
    slot->pdev.id.vendor     = vendor;
    slot->pdev.id.device     = device;

    int ret = platform_device_register(&slot->pdev);
    if (ret < 0) {
        kwarn("[DT] platform_device_register failed for \"%s\"\n", slot->name);
        return ret;
    }

    kinfo("[DT] %s -> %04x:%02x %s mmio=0x%lx irq=%s%d\n", slot->name,
          vendor, device, slot->res[0].type == RES_MMIO ? "mmio" : "irq",
          (unsigned long)slot->res[0].start,
          has_irq ? "" : "none ", has_irq ? (int)irq : 0);
    g_dt_slot_count++;
    return 0;
}

int riscv64_fdt_enumerate_platform_devices(void)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)__boot_dtb_ptr;
    if (!base || dt_read32(base) != FDT_MAGIC) {
        printf("[DT] no device tree handed over by firmware\n");
        return 0;
    }

    uint32_t totalsize    = dt_read32(base + 4);
    uint32_t off_struct   = dt_read32(base + 8);
    uint32_t off_strings  = dt_read32(base + 12);
    if (totalsize < 40 || off_struct >= totalsize || off_strings >= totalsize) {
        printf("[DT] malformed header, no devices enumerated\n");
        return 0;
    }

    const uint8_t *p        = base + off_struct;
    const uint8_t *endp     = base + totalsize;
    const uint8_t *stringsp = base + off_strings;
    int depth = 0;

    uint32_t addr_cells = DT_DEFAULT_ADDRESS_CELLS;
    uint32_t size_cells = DT_DEFAULT_SIZE_CELLS;

    /* current node */
    char     node_name[48];
    char     compatible[96];
    uint32_t compatible_len = 0;
    uint64_t mmio_base = 0, mmio_size = 0;
    uint32_t irq = 0;
    int      has_irq = 0;
    int      disabled = 0;

    int emitted_before = g_dt_slot_count;

    while (p + 4 <= endp) {
        uint32_t token = dt_read32(p);
        p += 4;
        if (token == FDT_END)
            break;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            const char *name_end = name;
            while ((const uint8_t *)name_end < endp && *name_end)
                name_end++;
            if ((const uint8_t *)name_end >= endp)
                break;
            depth++;

            uint32_t nlen = (uint32_t)(name_end - name);
            if (nlen >= sizeof(node_name))
                nlen = sizeof(node_name) - 1;
            memcpy(node_name, name, nlen);
            node_name[nlen] = '\0';

            compatible_len = 0;
            mmio_base = mmio_size = 0;
            irq = 0;
            has_irq = 0;
            disabled = 0;

            p = (const uint8_t *)(((uintptr_t)(name_end + 1) + 3) & ~3UL);
            continue;
        }

        if (token == FDT_END_NODE) {
            uint32_t vendor = 0, device = 0;
            if (depth >= 2 && !disabled && compatible_len &&
                dt_claim_compatible(compatible, compatible_len,
                                    &vendor, &device))
                dt_slot_emit(node_name, strlen(node_name), mmio_base, mmio_size,
                             irq, has_irq, vendor, device);
            depth--;
            continue;
        }

        if (token == FDT_NOP)
            continue;

        if (token != FDT_PROP)
            continue;

        if (p + 8 > endp)
            break;
        uint32_t len      = dt_read32(p);
        uint32_t name_off = dt_read32(p + 4);
        p += 8;
        if (p + len > endp || name_off >= totalsize - off_strings)
            break;
        const char *pname = (const char *)(stringsp + name_off);
        const void *value = p;
        p = (const uint8_t *)(((uintptr_t)((const uint8_t *)value + len) + 3) & ~3UL);

        if (depth == 1 && len == 4) {
            if (strcmp(pname, "#address-cells") == 0)
                addr_cells = dt_read32(value);
            else if (strcmp(pname, "#size-cells") == 0)
                size_cells = dt_read32(value);
            continue;
        }

        if (strcmp(pname, "compatible") == 0) {
            uint32_t copy = len < sizeof(compatible) - 1
                                ? len : (uint32_t)sizeof(compatible) - 1;
            memcpy(compatible, value, copy);
            compatible_len = copy;
        } else if (strcmp(pname, "status") == 0 && len >= 9 &&
                   memcmp(value, "disabled", 9) == 0) {
            disabled = 1;
        } else if (strcmp(pname, "reg") == 0 && !mmio_size) {
            uint32_t cells = len / 4;
            uint32_t need = addr_cells + size_cells;
            if (cells >= need) {
                const uint32_t *raw = (const uint32_t *)value;
                mmio_base = dt_cells_to_u64(raw, addr_cells);
                mmio_size = dt_cells_to_u64(raw + addr_cells, size_cells);
            }
        } else if (strcmp(pname, "interrupts") == 0 && !has_irq) {
            /* One cell is an SPI interrupt, two are a PPI pair; the driver wants
             * the single number either way. */
            const uint32_t *raw = (const uint32_t *)value;
            if (len >= 8)
                irq = raw[1];
            else if (len >= 4)
                irq = raw[0];
            if (len >= 4)
                has_irq = 1;
        }
    }

    int n = g_dt_slot_count - emitted_before;
    if (n)
        kinfo("[DT] enumerated %d device%s from the device tree\n",
              n, n == 1 ? "" : "s");
    return n;
}

#endif /* CONFIG_RISCV64 */
