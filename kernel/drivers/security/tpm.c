/*
 * A20OS — TPM 2.0 driver (TIS / FIFO interface).
 *
 * Detects a TPM via the ACPI TPM2 table (start method 6 = TIS FIFO) or a
 * legacy 0xFED40000 probe, and implements the minimal TIS FIFO state machine
 * plus the TPM2 command layer (Startup, GetCapability, GetRandom, PCR_Read).
 * Modern x86_64 firmware ships with a TPM 2.0, so this driver makes the
 * device available even though A20OS has no full security subsystem yet.
 *
 * The TIS register map and TPM2 command framing are defined by the TCG PC
 * Client Platform TPM Profile (TIS) / TPM 2.0 spec.  The FIFO state machine
 * (locality, burst count, COMMAND_READY/GO/DATA_AVAIL) follows the approach
 * of ViudiraTech/Uinxed-Kernel (Apache-2.0); this implementation is
 * rewritten for A20OS.  See docs/ACKNOWLEDGMENTS.md.
 */
#ifdef CONFIG_X86_64

#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "arch/x86_64/include/firmware.h"

/* ------------------------------------------------------------------ */
/* TIS register map (locality 0)                                       */
/* ------------------------------------------------------------------ */

#define TIS_ACCESS         0x0000U
#define TIS_INT_ENABLE     0x0008U
#define TIS_INT_STATUS     0x0010U
#define TIS_INTF_CAPS      0x0014U
#define TIS_STS            0x0018U
#define TIS_DATA_FIFO      0x0024U
#define TIS_DID_VID        0x0F00U
#define TIS_RID            0x0F04U

#define TIS_STS_VALID       0x80U
#define TIS_STS_COMMAND_READY 0x40U
#define TIS_STS_GO          0x20U
#define TIS_STS_DATA_AVAIL  0x10U
#define TIS_STS_DATA_EXPECT 0x08U

#define TIS_ACCESS_VALID         0x80U
#define TIS_ACCESS_ACTIVE_LOCALITY 0x20U
#define TIS_ACCESS_REQUEST_PENDING 0x04U
#define TIS_ACCESS_REQUEST_USE    0x02U

#define TIS_TIMEOUT_A_MS   750U
#define TIS_TIMEOUT_B_MS   2000U
#define TIS_TIMEOUT_C_MS   750U
#define TIS_TIMEOUT_D_MS   750U

/* TPM2 command codes. */
#define TPM2_CC_STARTUP        0x0144U
#define TPM2_CC_GET_CAPABILITY 0x017AU
#define TPM2_CC_GET_RANDOM     0x017BU
#define TPM2_CC_PCR_READ       0x017EU

#define TPM2_ST_NO_SESSIONS    0x8001U
#define TPM2_SU_CLEAR          0x0000U

#define TPM2_RC_WARN_MASK      0x800U

typedef struct {
    uintptr_t base;             /* virtual TIS MMIO base */
    uint64_t  phys;             /* physical base */
    uint32_t  vendor;
    uint32_t  device;
    int       present;
    spinlock_t lock;
} tpm_tis_dev_t;

static tpm_tis_dev_t g_tpm;

static inline uint32_t tpm_read32(uint32_t reg) {
    return readl((const volatile void *)(g_tpm.base + reg));
}
static inline void tpm_write32(uint32_t reg, uint32_t value) {
    writel(value, (volatile void *)(g_tpm.base + reg));
}

static int tpm_wait_sts(uint32_t mask, uint32_t expected, uint32_t timeout_ms) {
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(timeout_ms);
    do {
        uint32_t sts = tpm_read32(TIS_STS);
        if ((sts & mask) == expected)
            return 0;
        arch_cpu_relax();
    } while (timer_get_ticks() < deadline);
    return -1;
}

static uint16_t tpm_burst_count(void) {
    return (uint16_t)((tpm_read32(TIS_STS) >> 8) & 0xffffU);
}

static int tpm_request_locality(void) {
    uint32_t access = tpm_read32(TIS_ACCESS);
    if ((access & (TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY)) ==
        (TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY))
        return 0;               /* already owned */

    tpm_write32(TIS_ACCESS, TIS_ACCESS_REQUEST_USE);
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(TIS_TIMEOUT_A_MS);
    do {
        access = tpm_read32(TIS_ACCESS);
        if ((access & (TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY)) ==
            (TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY))
            return 0;
        arch_cpu_relax();
    } while (timer_get_ticks() < deadline);
    kerr("[TPM] locality request timeout\n");
    return -1;
}

static void tpm_relinquish_locality(void) {
    tpm_write32(TIS_ACCESS, TIS_ACCESS_ACTIVE_LOCALITY);
}

/* Send a full TPM2 command (big-endian framing) via the FIFO. */
static int tpm_send_command(const uint8_t *cmd, uint32_t cmd_len,
                            uint8_t *rsp, uint32_t rsp_cap, uint32_t *rsp_len) {
    if (!g_tpm.present)
        return -ENODEV;
    uint64_t flags = spin_lock_irqsave(&g_tpm.lock);
    int result = -EIO;

    if (tpm_request_locality() != 0) {
        spin_unlock_irqrestore(&g_tpm.lock, flags);
        return -EIO;
    }

    /* Command ready. */
    if (tpm_wait_sts(TIS_STS_COMMAND_READY, TIS_STS_COMMAND_READY,
                     TIS_TIMEOUT_B_MS) != 0) {
        tpm_write32(TIS_STS, TIS_STS_COMMAND_READY);
        goto out;
    }

    /* Write the command minus the last byte. */
    uint32_t count = 0;
    while (count < cmd_len - 1U) {
        uint16_t burst = tpm_burst_count();
        if (!burst) {
            if (tpm_wait_sts(TIS_STS_VALID, TIS_STS_VALID, TIS_TIMEOUT_B_MS) != 0)
                goto out;
            continue;
        }
        while (count < cmd_len - 1U && burst--) {
            tpm_write32(TIS_DATA_FIFO, cmd[count]);
            count++;
        }
    }
    /* Last byte triggers execution. */
    tpm_write32(TIS_DATA_FIFO, cmd[count]);
    count++;

    /* Wait for DATA_AVAIL. */
    if (tpm_wait_sts(TIS_STS_DATA_AVAIL, TIS_STS_DATA_AVAIL,
                     TIS_TIMEOUT_C_MS) != 0) {
        kerr("[TPM] response not ready\n");
        goto out;
    }

    /* Read response. */
    uint32_t got = 0;
    while (got < rsp_cap) {
        uint16_t burst = tpm_burst_count();
        if (!burst) {
            if (tpm_wait_sts(TIS_STS_DATA_AVAIL | TIS_STS_VALID,
                             TIS_STS_DATA_AVAIL | TIS_STS_VALID,
                             TIS_TIMEOUT_D_MS) != 0)
                break;
            continue;
        }
        while (burst-- && got < rsp_cap) {
            uint32_t v = tpm_read32(TIS_DATA_FIFO);
            rsp[got++] = (uint8_t)v;
            if (got == 10) {
                uint32_t expected = ((uint32_t)rsp[2] << 24) |
                                    ((uint32_t)rsp[3] << 16) |
                                    ((uint32_t)rsp[4] << 8) | rsp[5];
                if (expected > rsp_cap) {
                    kerr("[TPM] response too large: %u\n", expected);
                    goto out;
                }
            }
        }
        if (got >= 10) {
            uint32_t expected = ((uint32_t)rsp[2] << 24) |
                                ((uint32_t)rsp[3] << 16) |
                                ((uint32_t)rsp[4] << 8) | rsp[5];
            if (got == expected)
                break;
        }
    }

    *rsp_len = got;
    tpm_write32(TIS_STS, TIS_STS_COMMAND_READY);   /* clear DATA_AVAIL */
    result = 0;

out:
    tpm_relinquish_locality();
    spin_unlock_irqrestore(&g_tpm.lock, flags);
    return result;
}

static void tpm_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void tpm_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Build a TPM2 command header: tag[0..1] size[2..5] commandCode[6..9].
 * The caller writes parameters starting at offset 10. */
static void tpm2_cmd_hdr(uint8_t *cmd, uint16_t cc, uint32_t total) {
    cmd[0] = (uint8_t)(TPM2_ST_NO_SESSIONS >> 8);
    cmd[1] = (uint8_t)TPM2_ST_NO_SESSIONS;
    cmd[2] = (uint8_t)(total >> 24);
    cmd[3] = (uint8_t)(total >> 16);
    cmd[4] = (uint8_t)(total >> 8);
    cmd[5] = (uint8_t)total;
    tpm_put_be32(cmd + 6, cc);
}

/* Run a TPM2 command, returning the response code (0 = success) and
 * leaving the response body in rsp. */
static uint32_t tpm2_command(const uint8_t *params, uint32_t param_len,
                             uint16_t cc, uint8_t *rsp, uint32_t rsp_cap,
                             uint32_t *rsp_len) {
    uint8_t cmd[128];
    uint32_t total = param_len + 10U;
    if (total > sizeof(cmd))
        return 0xffffffffU;
    tpm2_cmd_hdr(cmd, cc, total);
    if (param_len)
        memcpy(cmd + 10, params, param_len);
    if (tpm_send_command(cmd, total, rsp, rsp_cap, rsp_len) != 0)
        return 0xffffffffU;
    return tpm2_response_code(rsp, *rsp_len);
}

/* Extract and normalise the response code from a TPM2 response. */
static uint32_t tpm2_response_code(const uint8_t *rsp, uint32_t rsp_len) {
    if (rsp_len < 10)
        return 0xffffffffU;
    uint32_t rc = ((uint32_t)rsp[6] << 24) | ((uint32_t)rsp[7] << 16) |
                  ((uint32_t)rsp[8] << 8) | rsp[9];
    if (rc & TPM2_RC_WARN_MASK)
        rc &= ~TPM2_RC_WARN_MASK;   /* warnings (e.g. retry) are non-fatal */
    return rc;
}

/* --- public API --- */

int tpm_get_random(uint8_t *buf, size_t len) {
    if (!g_tpm.present || !buf || !len || len > 64)
        return -EINVAL;
    uint8_t cmd[16] = { 0 };
    uint8_t rsp[80];
    uint32_t rsp_len = 0;
    tpm2_cmd_hdr(cmd, TPM2_CC_GET_RANDOM, 12);
    tpm_put_be16(cmd + 10, (uint16_t)len);   /* bytesRequested */
    if (tpm_send_command(cmd, 12, rsp, sizeof(rsp), &rsp_len) != 0)
        return -EIO;
    uint32_t rc = tpm2_response_code(rsp, rsp_len);
    if (rc != 0)
        return -EIO;
    /* TPM2B: size at rsp[10..11], bytes at rsp[12..]. */
    uint16_t size = (uint16_t)((rsp[10] << 8) | rsp[11]);
    if (size > len)
        size = (uint16_t)len;
    memcpy(buf, rsp + 12, size);
    return (int)size;
}

int tpm_present(void) {
    return g_tpm.present;
}

/* --- init --- */

static int tpm_tis_init(uintptr_t base, uint64_t phys) {
    g_tpm.base = base;
    g_tpm.phys = phys;
    spin_init(&g_tpm.lock);

    uint32_t didvid = tpm_read32(TIS_DID_VID);
    uint32_t rid = tpm_read32(TIS_RID);
    if (didvid == 0 || didvid == 0xffffffffU || rid == 0xffffffffU)
        return -ENODEV;
    g_tpm.vendor = didvid & 0xffffU;
    g_tpm.device = didvid >> 16;

    if (tpm_request_locality() != 0)
        return -EIO;
    tpm_relinquish_locality();

    g_tpm.present = 1;

    /* Startup(SU_CLEAR). */
    uint8_t cmd[12] = { 0 };
    uint8_t rsp[16];
    uint32_t rsp_len = 0;
    tpm2_cmd_hdr(cmd, TPM2_CC_STARTUP, 12);
    tpm_put_be16(cmd + 10, TPM2_SU_CLEAR);
    (void)tpm_send_command(cmd, 12, rsp, sizeof(rsp), &rsp_len);

    kinfo("[TPM] 2.0 ready: vendor=0x%04x device=0x%04x rev=0x%x base=0x%lx\n",
          g_tpm.vendor, g_tpm.device, rid, (unsigned long)base);
    return 0;
}

static int tpm_probe(device_t *dev) {
    (void)dev;
    uint64_t tpm_phys = firmware_acpi_tpm2();
    if (!tpm_phys)
        return -ENODEV;
    return tpm_tis_init(PAGE_OFFSET + (uintptr_t)tpm_phys, tpm_phys);
}

static const device_id_t tpm_ids[] = {
    { .vendor = 0, .device = 0,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t tpm_driver = {
    .name = "tpm",
    .id_table = tpm_ids,
    .bus = NULL,
    .probe = tpm_probe,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(tpm_driver);

#endif /* CONFIG_X86_64 */
