/*
 * TPM 2.0 driver (TIS / FIFO) — drvmod module (x86_64).
 *
 * Migrated from kernel/drivers/security/tpm.c (removed). The module binds
 * the kernel-registered "tpm" fixed device and discovers the TPM through
 * the MMIO resource supplied by the platform device.  With no TPM attached the
 * probe returns -ENODEV and the module stays idle.
 */

#include "drvmod/drvmod.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_SECURITY, "tpm2-tis", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           0x54504D00UL, 0));

#include "core/klog.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/types.h"
#include "core/defs.h"
#include "core/errno.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_hwapi.h"

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

#define TPM2_RC_INITIALIZE     0x100U
#define TIS_GLOBAL_INT_ENABLE  (1U << 31)

typedef struct {
    uintptr_t base;             /* virtual TIS MMIO base */
    uint64_t  phys;             /* physical base */
    uint32_t  vendor;
    uint32_t  device;
    int       present;
    int       closing, touched, locality_owned, locality_requested;
    device_t *owner;
} tpm_tis_dev_t;

static tpm_tis_dev_t g_tpm;
/* Polling with IRQs disabled prevents the tick deadline from advancing. */
static mutex_t tpm_lock = MUTEX_INIT;
static mutex_t tpm_owner_lock = MUTEX_INIT;
static int tpm_stop(void *context);

static inline uint8_t tpm_read8(uint32_t reg) {
    return readb((const volatile void *)(g_tpm.base + reg));
}
static inline void tpm_write8(uint32_t reg, uint8_t value) {
    writeb(value, (volatile void *)(g_tpm.base + reg));
}

static inline uint32_t tpm_read32(uint32_t reg) {
    return readl((const volatile void *)(g_tpm.base + reg));
}
static inline void tpm_write32(uint32_t reg, uint32_t value) {
    writel(value, (volatile void *)(g_tpm.base + reg));
}

static int tpm_wait8(uint32_t reg, uint8_t mask, uint8_t expected,
                     uint32_t timeout_ms, int cancelable) {
    uint64_t start = timer_get_ticks();
    do {
        if (cancelable && __atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE))
            return -ECANCELED;
        uint8_t value = tpm_read8(reg);
        if (value == 0xffU) return -ENODEV;
        if ((value & mask) == expected)
            return 0;
        arch_cpu_relax();
    } while (timer_get_ticks() - start < MS_TO_TICKS(timeout_ms));
    return -ETIMEDOUT;
}

static int tpm_burst_count(void) {
    uint64_t start = timer_get_ticks();
    do {
        if (__atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE)) return -ECANCELED;
        uint32_t status = tpm_read32(TIS_STS);
        if ((status & 0xffU) == 0xffU) return -ENODEV;
        int burst = (status >> 8) & 0xffffU;
        if (burst) return burst;
        arch_cpu_relax();
    } while (timer_get_ticks() - start < MS_TO_TICKS(TIS_TIMEOUT_A_MS));
    return -ETIMEDOUT;
}

static int tpm_request_locality(int cancelable) {
    if (cancelable && __atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE))
        return -ECANCELED;
    uint8_t access = tpm_read8(TIS_ACCESS);
    if (access == 0xffU) return -ENODEV;
    g_tpm.touched = 1;
    uint8_t mask = TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY | TIS_ACCESS_REQUEST_USE;
    if ((access & mask) != (TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY)) {
        g_tpm.locality_requested = 1;
        tpm_write8(TIS_ACCESS, TIS_ACCESS_REQUEST_USE);
        int result = tpm_wait8(TIS_ACCESS, mask,
            TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY, TIS_TIMEOUT_A_MS, cancelable);
        if (result) return result;
    }
    g_tpm.locality_owned = 1;
    return 0;
}

static int tpm_mask_irqs(void) {
    uint32_t enabled = tpm_read32(TIS_INT_ENABLE);
    if (enabled == 0xffffffffU) return -ENODEV;
    tpm_write32(TIS_INT_ENABLE, enabled & ~TIS_GLOBAL_INT_ENABLE);
    enabled = tpm_read32(TIS_INT_ENABLE);
    if (enabled == 0xffffffffU) return -ENODEV;
    return enabled & TIS_GLOBAL_INT_ENABLE ? -EIO : 0;
}

/* Abort/clear the FIFO before relinquishing locality. Failure keeps the
 * ownership record and mapping for a checked stop retry. Called under lock. */
static int tpm_abort_and_release(void) {
    if (!g_tpm.locality_owned)
        return g_tpm.locality_requested ? -EBUSY : 0;
    tpm_write8(TIS_STS, TIS_STS_COMMAND_READY);
    int result = tpm_wait8(TIS_STS,
        TIS_STS_COMMAND_READY | TIS_STS_DATA_AVAIL | TIS_STS_DATA_EXPECT,
        TIS_STS_COMMAND_READY, TIS_TIMEOUT_B_MS, 0);
    if (result) return result;
    tpm_write8(TIS_ACCESS, TIS_ACCESS_ACTIVE_LOCALITY);
    result = tpm_wait8(TIS_ACCESS,
        TIS_ACCESS_VALID | TIS_ACCESS_ACTIVE_LOCALITY | TIS_ACCESS_REQUEST_USE,
        TIS_ACCESS_VALID, TIS_TIMEOUT_A_MS, 0);
    if (!result) {
        g_tpm.locality_owned = 0;
        g_tpm.locality_requested = 0;
    }
    return result;
}

static uint32_t tpm_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int tpm_recv_bytes(uint8_t *rsp, uint32_t length) {
    uint32_t count = 0;
    while (count < length) {
        int result = tpm_wait8(TIS_STS, TIS_STS_VALID | TIS_STS_DATA_AVAIL,
            TIS_STS_VALID | TIS_STS_DATA_AVAIL, TIS_TIMEOUT_C_MS, 1);
        if (result) return result;
        int burst = tpm_burst_count();
        if (burst < 0) return burst;
        while (burst-- && count < length)
            rsp[count++] = tpm_read8(TIS_DATA_FIFO);
    }
    return 0;
}

/* Caller serializes both init and public commands. FIFO accesses are bytes;
 * STS/burst and interrupt registers retain their specified read widths. */
static int tpm_exchange_locked(const uint8_t *cmd, uint32_t cmd_len,
                               uint8_t *rsp, uint32_t rsp_cap, uint32_t *rsp_len) {
    if (!cmd || cmd_len < 10 || !rsp || rsp_cap < 10 || !rsp_len ||
        tpm_be32(cmd + 2) != cmd_len) return -EINVAL;
    *rsp_len = 0;
    int result = tpm_request_locality(1);
    if (result) goto out;
    result = tpm_mask_irqs();
    if (result) {
        __atomic_store_n(&g_tpm.closing, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_tpm.present, 0, __ATOMIC_RELEASE);
        goto out;
    }
    tpm_write8(TIS_STS, TIS_STS_COMMAND_READY);
    result = tpm_wait8(TIS_STS, TIS_STS_COMMAND_READY, TIS_STS_COMMAND_READY,
                       TIS_TIMEOUT_B_MS, 1);
    if (result) goto out;

    uint32_t count = 0;
    while (count < cmd_len - 1U) {
        int burst = tpm_burst_count();
        if (burst < 0) { result = burst; goto out; }
        while (burst-- && count < cmd_len - 1U)
            tpm_write8(TIS_DATA_FIFO, cmd[count++]);
        result = tpm_wait8(TIS_STS, TIS_STS_VALID, TIS_STS_VALID, TIS_TIMEOUT_C_MS, 1);
        if (result) goto out;
        if (!(tpm_read8(TIS_STS) & TIS_STS_DATA_EXPECT)) {
            result = -EIO;
            goto out;
        }
    }
    tpm_write8(TIS_DATA_FIFO, cmd[count]);
    result = tpm_wait8(TIS_STS, TIS_STS_VALID, TIS_STS_VALID, TIS_TIMEOUT_C_MS, 1);
    if (result) goto out;
    if (tpm_read8(TIS_STS) & TIS_STS_DATA_EXPECT) { result = -EIO; goto out; }
    tpm_write8(TIS_STS, TIS_STS_GO);
    result = tpm_wait8(TIS_STS, TIS_STS_VALID | TIS_STS_DATA_AVAIL,
        TIS_STS_VALID | TIS_STS_DATA_AVAIL, TIS_TIMEOUT_B_MS, 1);
    if (result) goto out;

    result = tpm_recv_bytes(rsp, 10);
    if (result) goto out;
    uint32_t expected = tpm_be32(rsp + 2);
    if (expected < 10 || expected > rsp_cap ||
        (((uint16_t)rsp[0] << 8) | rsp[1]) != TPM2_ST_NO_SESSIONS) {
        result = -EIO;
        goto out;
    }
    result = tpm_recv_bytes(rsp + 10, expected - 10);
    if (result) goto out;
    result = tpm_wait8(TIS_STS, TIS_STS_VALID, TIS_STS_VALID, TIS_TIMEOUT_C_MS, 1);
    if (result) goto out;
    if (tpm_read8(TIS_STS) & TIS_STS_DATA_AVAIL) { result = -EIO; goto out; }
    *rsp_len = expected;
out:
    {
        int cleanup = tpm_abort_and_release();
        if (cleanup) {
            __atomic_store_n(&g_tpm.closing, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_tpm.present, 0, __ATOMIC_RELEASE);
            if (!result) result = cleanup;
        }
    }
    if (result) *rsp_len = 0;
    return result;
}

static int tpm_send_command(const uint8_t *cmd, uint32_t cmd_len,
                            uint8_t *rsp, uint32_t rsp_cap, uint32_t *rsp_len) {
    mutex_lock(&tpm_lock);
    int result = !__atomic_load_n(&g_tpm.present, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE) ? -ENODEV :
        tpm_exchange_locked(cmd, cmd_len, rsp, rsp_cap, rsp_len);
    mutex_unlock(&tpm_lock);
    return result;
}

static void tpm_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void tpm_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Build a TPM2 command header: tag[0..1] size[2..5] commandCode[6..9].
 * The caller writes parameters starting at offset 10. */
static void tpm2_cmd_hdr(uint8_t *cmd, uint32_t cc, uint32_t total) {
    cmd[0] = (uint8_t)(TPM2_ST_NO_SESSIONS >> 8);
    cmd[1] = (uint8_t)TPM2_ST_NO_SESSIONS;
    cmd[2] = (uint8_t)(total >> 24);
    cmd[3] = (uint8_t)(total >> 16);
    cmd[4] = (uint8_t)(total >> 8);
    cmd[5] = (uint8_t)total;
    tpm_put_be32(cmd + 6, cc);
}

/* Nonzero TPM response codes, including warnings/retry, remain errors. */
static uint32_t tpm2_response_code(const uint8_t *rsp, uint32_t rsp_len);

/* Run a TPM2 command, returning the response code (0 = success) and
 * leaving the response body in rsp. */
static __attribute__((unused)) uint32_t tpm2_command(const uint8_t *params, uint32_t param_len,
                             uint32_t cc, uint8_t *rsp, uint32_t rsp_cap,
                             uint32_t *rsp_len) {
    uint8_t cmd[128];
    if (param_len > sizeof(cmd) - 10U || (param_len && !params))
        return 0xffffffffU;
    uint32_t total = param_len + 10U;
    tpm2_cmd_hdr(cmd, cc, total);
    if (param_len)
        memcpy(cmd + 10, params, param_len);
    if (tpm_send_command(cmd, total, rsp, rsp_cap, rsp_len) != 0)
        return 0xffffffffU;
    return tpm2_response_code(rsp, *rsp_len);
}

static uint32_t tpm2_response_code(const uint8_t *rsp, uint32_t rsp_len) {
    if (!rsp || rsp_len < 10 || tpm_be32(rsp + 2) != rsp_len ||
        (((uint16_t)rsp[0] << 8) | rsp[1]) != TPM2_ST_NO_SESSIONS)
        return 0xffffffffU;
    return tpm_be32(rsp + 6);
}

/* --- public API --- */

int tpm_get_random(uint8_t *buf, size_t len) {
    if (!buf || !len || len > 64)
        return -EINVAL;
    uint8_t cmd[16] = { 0 };
    uint8_t rsp[80];
    uint32_t rsp_len = 0;
    tpm2_cmd_hdr(cmd, TPM2_CC_GET_RANDOM, 12);
    tpm_put_be16(cmd + 10, (uint16_t)len);   /* bytesRequested */
    int result = tpm_send_command(cmd, 12, rsp, sizeof(rsp), &rsp_len);
    if (result) return result;
    uint32_t rc = tpm2_response_code(rsp, rsp_len);
    if (rc != 0 || rsp_len < 12)
        return -EIO;
    /* TPM2B: size at rsp[10..11], bytes at rsp[12..]. */
    uint16_t size = (uint16_t)((rsp[10] << 8) | rsp[11]);
    if (size > len || size != rsp_len - 12U)
        return -EIO;
    memcpy(buf, rsp + 12, size);
    return (int)size;
}

int tpm_present(void) {
    return __atomic_load_n(&g_tpm.present, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE);
}

/* --- init --- */

static int tpm_tis_init(uintptr_t base, uint64_t phys) {
    g_tpm.base = base;
    g_tpm.phys = phys;

    uint32_t didvid = tpm_read32(TIS_DID_VID);
    uint8_t rid = tpm_read8(TIS_RID);
    if (didvid == 0 || didvid == 0xffffffffU || rid == 0xffU)
        return -ENODEV;
    g_tpm.vendor = didvid & 0xffffU;
    g_tpm.device = didvid >> 16;

    /* Startup(SU_CLEAR). */
    uint8_t cmd[12] = { 0 };
    uint8_t rsp[16];
    uint32_t rsp_len = 0;
    tpm2_cmd_hdr(cmd, TPM2_CC_STARTUP, 12);
    tpm_put_be16(cmd + 10, TPM2_SU_CLEAR);
    int result = tpm_exchange_locked(cmd, 12, rsp, sizeof(rsp), &rsp_len);
    if (result) return result;
    uint32_t rc = tpm2_response_code(rsp, rsp_len);
    /* Firmware may already have issued Startup. No other code is success. */
    if (rsp_len != 10 || (rc && rc != TPM2_RC_INITIALIZE)) return -EIO;
    if (__atomic_load_n(&g_tpm.closing, __ATOMIC_ACQUIRE)) return -ECANCELED;
    __atomic_store_n(&g_tpm.present, 1, __ATOMIC_RELEASE);

    kinfo("[TPM] 2.0 ready: vendor=0x%04x device=0x%04x rev=0x%x base=0x%lx\n",
          g_tpm.vendor, g_tpm.device, rid, (unsigned long)base);
    return 0;
}

static int tpm_probe_locked(device_t *dev)
{
    if (g_tpm.owner || g_tpm.base) return -EBUSY;
    if (fw_node_match_id(&dev->firmware_node, "MSFT0101") > 0) {
        uint32_t method;
        if (fw_node_read_u32(&dev->firmware_node, "acpi,tpm2-start-method", &method))
            return -ENODEV;
        if (method != 6) return -EOPNOTSUPP; /* CRB has a different protocol. */
    }
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start || res->end - res->start < TIS_RID + 3)
        return -ENODEV;
    int result = device_probe_use_remove(dev);
    if (result) return result;
    g_tpm.owner = dev;
    g_tpm.touched = 0;
    g_tpm.locality_owned = 0;
    g_tpm.locality_requested = 0;
    __atomic_store_n(&g_tpm.closing, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tpm.present, 0, __ATOMIC_RELEASE);
    dev->drv_priv = &g_tpm;
    result = device_add_execution_action(dev, tpm_stop, NULL, dev);
    if (result) return result;
    uintptr_t mapping = (uintptr_t)device_map_mmio(dev, 0);
    return mapping ? tpm_tis_init(mapping, res->start) : -EIO;
}

static int tpm_probe(device_t *dev)
{
    mutex_lock(&tpm_owner_lock);
    mutex_lock(&tpm_lock);
    int result = tpm_probe_locked(dev);
    mutex_unlock(&tpm_lock);
    mutex_unlock(&tpm_owner_lock);
    return result;
}

static int tpm_stop(void *context)
{
    device_t *dev = context;
    if (!dev || !dev->drv_priv) return 0;
    __atomic_store_n(&g_tpm.closing, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tpm.present, 0, __ATOMIC_RELEASE);
    if (!mutex_trylock(&tpm_owner_lock)) return -EBUSY;
    if (!mutex_trylock(&tpm_lock)) {
        mutex_unlock(&tpm_owner_lock);
        return -EBUSY;
    }
    int result = g_tpm.owner == dev ? 0 : -ENODEV;
    if (!result && g_tpm.base && g_tpm.touched) {
        /* Acquisition, IRQ mask, abort and locality release all acknowledge
         * before core may discard this binding's managed MMIO mapping. */
        result = tpm_request_locality(0);
        if (!result) result = tpm_mask_irqs();
        if (!result) result = tpm_abort_and_release();
    }
    mutex_unlock(&tpm_lock);
    mutex_unlock(&tpm_owner_lock);
    return result;
}

static int tpm_remove(device_t *dev)
{
    if (!dev || !dev->drv_priv) return 0;
    int result = tpm_stop(dev);
    if (result) return result;
    mutex_lock(&tpm_owner_lock);
    mutex_lock(&tpm_lock);
    g_tpm.base = 0;
    g_tpm.owner = NULL;
    dev->drv_priv = NULL;
    mutex_unlock(&tpm_lock);
    mutex_unlock(&tpm_owner_lock);
    return 0;
}

static const firmware_device_id_t firmware_ids[] = {
    {.id = "MSFT0101"}, {.id = "tcg,tpm-tis-mmio"}, {0}
};
static driver_t tpm_driver = {
    .name = "tpm2-tis",
    .firmware_ids = firmware_ids,
    .bus = &platform_bus,
    .probe = tpm_probe,
    .remove = tpm_remove,
    .class_type = DEV_CLASS_NONE,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&tpm_driver);
    drv_log("[TPM] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
