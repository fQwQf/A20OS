/*
 * A20OS I2C master for DesignWare APB I2C controllers (Synopsys DW_apb_i2c).
 *
 * The AMD I2C controller a laptop's I2C-HID touchpad hangs off enumerates as
 * ACPI "AMDI0010"/"AMDI0011", which Linux binds to i2c-designware; the same
 * register file is described by "snps,designware-i2c" in device tree.
 *
 * Register offsets, IC_CON bit positions, the SCL timing formula and the
 * enable/disable handshake follow Linux's
 * drivers/i2c/busses/i2c-designware-{common,master,core.h} and
 * include/linux/designware_i2c.h.  Three details there are easy to get wrong
 * and are called out at the use site: IC_CON.RESTART_EN is bit 5 (not 6),
 * IC_TAR is only latched while the controller is disabled, and a read longer
 * than the RX FIFO has to be drained while it is being pushed.
 *
 * Polled master only: no interrupts, no slave mode, no SMBus block transfers.
 * That covers I2C-HID, whose transfers are short register reads/writes, and
 * keeps the driver usable when the attention GPIO is not routed.
 *
 * The SCL timing registers need the controller's input clock (IC_CLK).  This
 * platform has no clock provider to ask, so the driver assumes the 100 MHz IC
 * clock the AMD integration uses and exposes a20.i2c.ic_clk=<Hz> to correct
 * it.  A wrong assumption produces a wrong SCL rate, not memory corruption:
 * a slave that cannot answer makes the transfer time out and the client
 * driver reports the device as absent.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/i2c/i2c.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/mm.h"

/* ---- register file (DW_apb_i2c databook) --------------------------------- */
#define DW_IC_CON            0x00U
#define DW_IC_TAR            0x04U
#define DW_IC_DATA_CMD       0x10U
#define DW_IC_SS_SCL_HCNT    0x14U
#define DW_IC_SS_SCL_LCNT    0x18U
#define DW_IC_FS_SCL_HCNT    0x1CU
#define DW_IC_FS_SCL_LCNT    0x20U
#define DW_IC_INTR_STAT      0x2CU
#define DW_IC_INTR_MASK      0x30U
#define DW_IC_RAW_INTR_STAT  0x34U
#define DW_IC_RX_TL          0x38U
#define DW_IC_TX_TL          0x3CU
#define DW_IC_CLR_INTR       0x40U
#define DW_IC_CLR_RX_UNDER   0x44U
#define DW_IC_CLR_RX_OVER    0x48U
#define DW_IC_CLR_TX_OVER    0x4CU
#define DW_IC_CLR_TX_ABRT    0x54U
#define DW_IC_CLR_ACTIVITY   0x5CU
#define DW_IC_CLR_STOP_DET   0x60U
#define DW_IC_CLR_START_DET  0x64U
#define DW_IC_ENABLE         0x6CU
#define DW_IC_STATUS         0x70U
#define DW_IC_TXFLR          0x74U
#define DW_IC_RXFLR          0x78U
#define DW_IC_TX_ABRT_SOURCE 0x80U
#define DW_IC_ENABLE_STATUS  0x9CU
#define DW_IC_COMP_PARAM_1   0xF4U
#define DW_IC_COMP_TYPE      0xFCU

/* IC_CON bits (include/linux/designware_i2c.h). */
#define DW_CON_MASTER         0x00000001U
#define DW_CON_SPEED_STD      0x00000002U /* bits 2:1 = 01 */
#define DW_CON_SPEED_FAST     0x00000004U /* bits 2:1 = 10 */
#define DW_CON_RESTART_EN     0x00000020U /* bit 5 */
#define DW_CON_SLAVE_DISABLE  0x00000040U /* bit 6 */

/* IC_STATUS bits. */
#define DW_STATUS_ACTIVITY      0x00000001U
#define DW_STATUS_TFNF          0x00000002U /* Tx FIFO not full */
#define DW_STATUS_TFE           0x00000004U /* Tx FIFO empty */
#define DW_STATUS_RFNE          0x00000008U /* Rx FIFO not empty */
#define DW_STATUS_MST_ACTIVITY  0x00000020U

/* IC_DATA_CMD bits. */
#define DW_CMD_READ     0x00000100U /* bit 8 */
#define DW_CMD_STOP     0x00000200U /* bit 9 */
#define DW_CMD_RESTART  0x00000400U /* bit 10 */
#define DW_ENABLE_ABORT 0x00000002U

/* IC_RAW_INTR_STAT / IC_TX_ABRT_SOURCE bits we act on. */
#define DW_INTR_TX_ABRT        0x00000040U
#define DW_INTR_STOP_DET       0x00000200U
#define DW_ABRT_7B_ADDR_NOACK  0x00000001U
#define DW_ABRT_10ADDR1_NOACK  0x00000002U
#define DW_ABRT_10ADDR2_NOACK  0x00000004U
#define DW_ABRT_TXDATA_NOACK   0x00000008U
#define DW_ABRT_MASTER_DIS     0x00000800U
#define DW_ABRT_ARB_LOST       0x00001000U

/* DW_IC_COMP_TYPE_VALUE = "DW_apb_i2c" (Linux rejects anything else so a
 * wrong ACPI memory descriptor cannot be driven as this controller). */
#define DW_COMP_TYPE_VALUE   0x44570140U

#define DW_I2C_TIMEOUT_US    20000U /* per FIFO wait, 20 ms */
#define DW_I2C_XFER_US      200000U /* whole message, 200 ms */

#define DW_SIM_REGS 0x100U

/* Model of the controller's register file and of the slave on the bus.  Only
 * used by the boot self-test; see the model section below. */
struct dw_i2c_sim;
struct dw_i2c_slave;

typedef struct dw_i2c_controller {
    uintptr_t regs;
    /* Non-NULL in the self-test: accessors talk to the model instead of MMIO. */
    struct dw_i2c_sim *sim;
    struct dw_i2c_slave *slave;
    uint32_t ic_clk_hz;
    uint32_t bus_speed_hz;
    uint32_t tx_fifo_depth;
    uint32_t rx_fifo_depth;
    int failure_logged;
    unsigned recovery_count;
    mutex_t lock;
    i2c_adapter_t adapter;
} dw_i2c_controller_t;

static uint32_t dw_sim_read(dw_i2c_controller_t *i2c, uint32_t reg);
static void dw_sim_write(dw_i2c_controller_t *i2c, uint32_t reg, uint32_t value);

static inline uint32_t dw_read(dw_i2c_controller_t *i2c, uint32_t reg)
{
    if (i2c->sim)
        return dw_sim_read(i2c, reg);
    return readl((const volatile void *)(i2c->regs + reg));
}

static inline void dw_write(dw_i2c_controller_t *i2c, uint32_t reg,
                            uint32_t value)
{
    if (i2c->sim) {
        dw_sim_write(i2c, reg, value);
        return;
    }
    writel(value, (volatile void *)(i2c->regs + reg));
}

/* Boot-arg overrides: the input clock and the target SCL rate are board
 * facts this driver cannot discover through the generic firmware interface. */
static uint32_t g_dw_ic_clk_hz = 100000000U;
static uint32_t g_dw_bus_speed_hz = 400000U;

static void dw_i2c_parse_cmdline(void)
{
    const char *cmdline = bootargs_get();
    for (const char *p = cmdline; p && *p; ) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t')
            end++;

        static const char ic_key[] = "a20.i2c.ic_clk=";
        static const char speed_key[] = "a20.i2c.speed=";
        if ((size_t)(end - p) > sizeof(ic_key) - 1 &&
            !memcmp(p, ic_key, sizeof(ic_key) - 1)) {
            uint32_t v = 0;
            for (const char *q = p + sizeof(ic_key) - 1;
                 q < end && *q >= '0' && *q <= '9'; q++)
                v = v * 10U + (uint32_t)(*q - '0');
            if (v >= 1000000U)
                g_dw_ic_clk_hz = v;
        } else if ((size_t)(end - p) > sizeof(speed_key) - 1 &&
                   !memcmp(p, speed_key, sizeof(speed_key) - 1)) {
            uint32_t v = 0;
            for (const char *q = p + sizeof(speed_key) - 1;
                 q < end && *q >= '0' && *q <= '9'; q++)
                v = v * 10U + (uint32_t)(*q - '0');
            if (v >= 10000U && v <= 1000000U)
                g_dw_bus_speed_hz = v;
        }
        p = end;
    }
}

/* Linux i2c_dw_scl_hcnt()/lcnt(): the register counts IC_CLK cycles for the
 * high/low phase plus the line's fall time, minus the fixed internal latency
 * (3 for high, 1 for low).  tf defaults to 300 ns there and here. */
static uint32_t dw_i2c_div_round(uint64_t num, uint64_t den)
{
    return (uint32_t)((num + den / 2U) / den);
}

static void dw_i2c_scl_timing(uint64_t ic_clk,
                              uint32_t *ss_hcnt, uint32_t *ss_lcnt,
                              uint32_t *fs_hcnt, uint32_t *fs_lcnt)
{
    const uint64_t tf_ns = 300U;
    /* Standard: tHIGH/tHD;STA 4.0 us, tLOW 4.7 us. */
    uint64_t ss_h = dw_i2c_div_round(ic_clk * (4000U + tf_ns), 1000000000ULL);
    uint64_t ss_l = dw_i2c_div_round(ic_clk * (4700U + tf_ns), 1000000000ULL);
    /* Fast: 0.6 us / 1.3 us. */
    uint64_t fs_h = dw_i2c_div_round(ic_clk * (600U + tf_ns), 1000000000ULL);
    uint64_t fs_l = dw_i2c_div_round(ic_clk * (1300U + tf_ns), 1000000000ULL);

    *ss_hcnt = (uint32_t)(ss_h > 3U ? ss_h - 3U : 1U);
    *ss_lcnt = (uint32_t)(ss_l > 1U ? ss_l - 1U : 1U);
    *fs_hcnt = (uint32_t)(fs_h > 3U ? fs_h - 3U : 1U);
    *fs_lcnt = (uint32_t)(fs_l > 1U ? fs_l - 1U : 1U);
}

static int dw_i2c_wait_enable(dw_i2c_controller_t *i2c, int enabled)
{
    for (unsigned i = 0; i < DW_I2C_TIMEOUT_US; i++) {
        uint32_t st = dw_read(i2c, DW_IC_ENABLE_STATUS) & 1U;
        if (!!st == !!enabled)
            return 0;
        udelay(1);
    }
    return -ETIMEDOUT;
}

static int dw_i2c_disable(dw_i2c_controller_t *i2c)
{
    dw_write(i2c, DW_IC_ENABLE, 0);
    return dw_i2c_wait_enable(i2c, 0);
}

static int dw_i2c_enable(dw_i2c_controller_t *i2c)
{
    dw_write(i2c, DW_IC_ENABLE, 1);
    int rc = dw_i2c_wait_enable(i2c, 1);
    /* Dummy read, as Linux does, to release a stuck register on some cores. */
    (void)dw_read(i2c, DW_IC_ENABLE_STATUS);
    return rc;
}

/* IC_ENABLE=0 alone need not stop an active master. Abort while ENABLE is
 * set, wait for the self-clearing ABORT bit, then disable and flush. This is
 * the DesignWare sequence used by Linux's __i2c_dw_disable(). */
static int dw_i2c_abort_transfer(dw_i2c_controller_t *i2c)
{
    if (dw_i2c_enable(i2c) < 0)
        return -ETIMEDOUT;
    dw_write(i2c, DW_IC_ENABLE, 1U | DW_ENABLE_ABORT);
    for (unsigned i = 0; i < 1000; i++) {
        if (!(dw_read(i2c, DW_IC_ENABLE) & DW_ENABLE_ABORT))
            return dw_i2c_disable(i2c);
        udelay(1);
    }
    return -ETIMEDOUT;
}

static void dw_i2c_clear_errors(dw_i2c_controller_t *i2c)
{
    (void)dw_read(i2c, DW_IC_CLR_INTR);
    (void)dw_read(i2c, DW_IC_CLR_RX_UNDER);
    (void)dw_read(i2c, DW_IC_CLR_RX_OVER);
    (void)dw_read(i2c, DW_IC_CLR_TX_OVER);
    (void)dw_read(i2c, DW_IC_CLR_TX_ABRT);
    (void)dw_read(i2c, DW_IC_CLR_STOP_DET);
    (void)dw_read(i2c, DW_IC_CLR_START_DET);
    (void)dw_read(i2c, DW_IC_CLR_ACTIVITY);
}

static int dw_i2c_abort_status(dw_i2c_controller_t *i2c)
{
    uint32_t src = dw_read(i2c, DW_IC_TX_ABRT_SOURCE);
    if (!src && !(dw_read(i2c, DW_IC_RAW_INTR_STAT) & DW_INTR_TX_ABRT))
        return 0;
    (void)dw_read(i2c, DW_IC_CLR_TX_ABRT);
    if (src & (DW_ABRT_7B_ADDR_NOACK | DW_ABRT_10ADDR1_NOACK |
               DW_ABRT_10ADDR2_NOACK))
        return -ENXIO;
    if (src & (DW_ABRT_TXDATA_NOACK | DW_ABRT_ARB_LOST))
        return -EIO;
    if (src & DW_ABRT_MASTER_DIS)
        return -ESHUTDOWN;
    return -EIO;
}

static int dw_i2c_wait_stop(dw_i2c_controller_t *i2c)
{
    for (unsigned i = 0; i < DW_I2C_TIMEOUT_US; i++) {
        int ret = dw_i2c_abort_status(i2c);
        if (ret)
            return ret;
        if (dw_read(i2c, DW_IC_RAW_INTR_STAT) & DW_INTR_STOP_DET)
            return 0;
        udelay(1);
    }
    return -ETIMEDOUT;
}

/* One write message: push every byte, STOP on the last one when this is the
 * last message of the transfer. */
static int dw_i2c_write_msg(dw_i2c_controller_t *i2c, const i2c_msg_t *msg,
                            int last)
{
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(200);
    for (uint16_t i = 0; i < msg->len; i++) {
        for (;;) {
            int ab = dw_i2c_abort_status(i2c);
            if (ab)
                return ab;
            if (dw_read(i2c, DW_IC_STATUS) & DW_STATUS_TFNF)
                break;
            if (clock_get_ticks() >= deadline)
                return -ETIMEDOUT;
            udelay(1);
        }
        uint32_t cmd = msg->buf[i];
        if (last && (uint16_t)(i + 1U) == msg->len)
            cmd |= DW_CMD_STOP;
        dw_write(i2c, DW_IC_DATA_CMD, cmd);
    }
    return 0;
}

/* One read message: queue READ commands and drain the RX FIFO while queuing.
 * The FIFO is much smaller than a report descriptor, so a push-everything
 * loop would fill RX, stall the core and never drain. */
static int dw_i2c_read_msg(dw_i2c_controller_t *i2c, i2c_msg_t *msg, int first,
                           int last)
{
    uint16_t pushed = 0, drained = 0;
    uint64_t deadline = clock_get_ticks() + MS_TO_TICKS(200);

    while (drained < msg->len) {
        uint32_t status = dw_read(i2c, DW_IC_STATUS);
        if (drained < pushed && (status & DW_STATUS_RFNE)) {
            msg->buf[drained++] =
                (uint8_t)(dw_read(i2c, DW_IC_DATA_CMD) & 0xffU);
            continue;
        }
        if (pushed < msg->len && (status & DW_STATUS_TFNF) &&
            (unsigned)(pushed - drained) < i2c->rx_fifo_depth) {
            uint32_t cmd = DW_CMD_READ;
            if (pushed == 0 && !first)
                cmd |= DW_CMD_RESTART;
            if (last && (uint16_t)(pushed + 1U) == msg->len)
                cmd |= DW_CMD_STOP;
            dw_write(i2c, DW_IC_DATA_CMD, cmd);
            pushed++;
            continue;
        }
        int ab = dw_i2c_abort_status(i2c);
        if (ab)
            return ab;
        if (clock_get_ticks() >= deadline)
            return -ETIMEDOUT;
        udelay(1);
    }
    return 0;
}

static int dw_i2c_transfer(i2c_adapter_t *adapter, i2c_msg_t *msgs, int n)
{
    dw_i2c_controller_t *i2c = adapter ? adapter->priv : NULL;
    if (!i2c || !msgs || n <= 0)
        return -EINVAL;
    if (!msgs[0].len)
        return 0;
    /* All messages of one transfer target the same slave; a transfer that
     * mixed addresses would need a new START per address, which this driver
     * does not implement. */
    for (int i = 1; i < n; i++)
        if (msgs[i].addr != msgs[0].addr || !msgs[i].len)
            return -EOPNOTSUPP;
    if (msgs[0].addr > 0x7fU)
        return -EINVAL; /* 10-bit addressing is not enabled */

    mutex_lock(&i2c->lock);
    int ret = 0;

    /* IC_TAR is latched when the controller is enabled, and RESTART_EN has to
     * be part of IC_CON before a write-then-read is issued.  Disable, program
     * the target, then enable: the same order as i2c_dw_xfer_init(). */
    if (dw_i2c_disable(i2c) < 0) {
        ret = -ETIMEDOUT;
        goto out;
    }
    dw_i2c_clear_errors(i2c);
    dw_write(i2c, DW_IC_TAR, msgs[0].addr);
    if (dw_i2c_enable(i2c) < 0) {
        ret = -ETIMEDOUT;
        goto out;
    }
    (void)dw_read(i2c, DW_IC_CLR_INTR);

    for (int i = 0; i < n && ret == 0; i++) {
        /* I2C_M_WRITE is zero: the read flag is the one that carries
         * information (same test as i2c-core and the Hi3861 adapter). */
        if (msgs[i].flags & I2C_M_READ)
            ret = dw_i2c_read_msg(i2c, &msgs[i], i == 0, i == n - 1);
        else
            ret = dw_i2c_write_msg(i2c, &msgs[i], i == n - 1);
    }
    /* TX FIFO availability only proves enqueue. Disabling before STOP can
     * discard a command that the slave has yet to acknowledge. */
    if (ret == 0)
        ret = dw_i2c_wait_stop(i2c);

out:
    if (ret == -ETIMEDOUT && !i2c->failure_logged) {
        i2c->failure_logged = 1;
        kwarn("[I2C-DW] first timeout: addr=%02x len=%u status=%02x raw=%04x tx=%u rx=%u\n",
              msgs[0].addr, msgs[n - 1].len, dw_read(i2c, DW_IC_STATUS),
              dw_read(i2c, DW_IC_RAW_INTR_STAT), dw_read(i2c, DW_IC_TXFLR),
              dw_read(i2c, DW_IC_RXFLR));
    }
    /* Linux disables the adapter between transfers (__i2c_dw_disable_nowait)
     * so the next one starts from a known idle state. */
    if (ret) {
        int recovered = dw_i2c_abort_transfer(i2c);
        if (ret == -ETIMEDOUT && (++i2c->recovery_count <= 5 || !(i2c->recovery_count % 16)))
            kinfo("[I2C-DW] timeout recovery: abort/disable result=%d\n", recovered);
    } else {
        (void)dw_i2c_disable(i2c);
    }
    if (ret)
        dw_i2c_clear_errors(i2c);
    mutex_unlock(&i2c->lock);
    return ret;
}

static int dw_i2c_program_timing(dw_i2c_controller_t *i2c);

/*
 * i2c_dw_init(): configure timing and IC_CON with the controller disabled, then
 * enable.  Shared by probe and the register-model self-test.
 */
static int dw_i2c_init_hw(dw_i2c_controller_t *i2c)
{
    if (dw_i2c_disable(i2c) < 0)
        return -ETIMEDOUT;
    dw_i2c_clear_errors(i2c);
    dw_write(i2c, DW_IC_INTR_MASK, 0);
    dw_i2c_program_timing(i2c);
    dw_write(i2c, DW_IC_TX_TL, i2c->tx_fifo_depth / 2U);
    dw_write(i2c, DW_IC_RX_TL, 0);

    /* i2c_dw_configure_master(): master, slave disabled, restarts enabled,
     * and the core's own speed select.  RESTART_EN is bit 5 -- an earlier
     * version of this driver placed it at bit 6 (SLAVE_DISABLE) and left
     * repeated-start off, which breaks every write-then-read. */
    uint32_t con = DW_CON_MASTER | DW_CON_SLAVE_DISABLE | DW_CON_RESTART_EN |
                   (i2c->bus_speed_hz > 100000U ? DW_CON_SPEED_FAST
                                               : DW_CON_SPEED_STD);
    dw_write(i2c, DW_IC_CON, con);
    return dw_i2c_enable(i2c);
}

static int dw_i2c_program_timing(dw_i2c_controller_t *i2c)
{
    uint32_t ss_h, ss_l, fs_h, fs_l;
    dw_i2c_scl_timing(i2c->ic_clk_hz, &ss_h, &ss_l, &fs_h, &fs_l);
    dw_write(i2c, DW_IC_SS_SCL_HCNT, ss_h);
    dw_write(i2c, DW_IC_SS_SCL_LCNT, ss_l);
    dw_write(i2c, DW_IC_FS_SCL_HCNT, fs_h);
    dw_write(i2c, DW_IC_FS_SCL_LCNT, fs_l);
    kinfo("[I2C-DW] timings: ic_clk=%u speed=%u ss=%u/%u fs=%u/%u\n",
          (unsigned)i2c->ic_clk_hz, (unsigned)i2c->bus_speed_hz,
          (unsigned)ss_h, (unsigned)ss_l, (unsigned)fs_h, (unsigned)fs_l);
    return 0;
}

static int dw_i2c_probe(device_t *dev)
{
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start ||
        res->end - res->start + 1U < DW_IC_COMP_TYPE + 4U)
        return -ENODEV;

    dw_i2c_parse_cmdline();

    dw_i2c_controller_t *i2c = kcalloc(1, sizeof(*i2c));
    if (!i2c)
        return -ENOMEM;
    i2c->regs = (uintptr_t)device_map_mmio(dev, 0);
    if (!i2c->regs) {
        kfree(i2c);
        return -EIO;
    }
    mutex_init(&i2c->lock);
    i2c->ic_clk_hz = g_dw_ic_clk_hz;
    i2c->bus_speed_hz = g_dw_bus_speed_hz;
    uint32_t hz;
    if (!fw_node_read_u32(&dev->firmware_node, "clock-frequency", &hz) &&
        hz >= 1000000U)
        i2c->ic_clk_hz = hz;
    if (!fw_node_read_u32(&dev->firmware_node, "i2c-bus-frequency", &hz) &&
        hz >= 10000U && hz <= 1000000U)
        i2c->bus_speed_hz = hz;

    /* Confirm the register file before touching anything else: ACPI memory
     * descriptors have been wrong before, and driving an unrelated block with
     * this register layout is not recoverable. */
    uint32_t type = dw_read(i2c, DW_IC_COMP_TYPE);
    if (type != DW_COMP_TYPE_VALUE) {
        kerr("[I2C-DW] component type 0x%08x != 0x%08x; not this "
             "controller\n", type, DW_COMP_TYPE_VALUE);
        kfree(i2c);
        return -ENODEV;
    }

    uint32_t comp = dw_read(i2c, DW_IC_COMP_PARAM_1);
    i2c->tx_fifo_depth = ((comp >> 16) & 0xffU) + 1U;
    i2c->rx_fifo_depth = ((comp >> 8) & 0xffU) + 1U;
    if (i2c->tx_fifo_depth < 2U || i2c->tx_fifo_depth > 256U)
        i2c->tx_fifo_depth = 8U;
    if (i2c->rx_fifo_depth < 2U || i2c->rx_fifo_depth > 256U)
        i2c->rx_fifo_depth = 8U;

    if (dw_i2c_init_hw(i2c) < 0) {
        kerr("[I2C-DW] controller did not initialise\n");
        kfree(i2c);
        return -EIO;
    }

    int nr = i2c_adapter_alloc_nr();
    if (nr < 0) {
        (void)dw_i2c_disable(i2c);
        kfree(i2c);
        return nr;
    }
    i2c->adapter.name = "i2c-designware";
    i2c->adapter.nr = nr;
    i2c->adapter.transfer = dw_i2c_transfer;
    i2c->adapter.priv = i2c;
    int rc = i2c_adapter_add_for_device(dev, &i2c->adapter);
    if (rc < 0) {
        (void)dw_i2c_disable(i2c);
        kfree(i2c);
        return rc;
    }
    dev->drv_priv = i2c;

    /* Leave the core disabled between transfers, as i2c_dw_init() does: an
     * enabled idle master keeps its clock divider running and some cores
     * latch the wrong target if the bus is touched before the next transfer
     * re-programs IC_TAR. */
    (void)dw_i2c_disable(i2c);

    kinfo("[I2C-DW] adapter %d ready: speed=%u Hz ic_clk=%u Hz "
          "fifo tx=%u rx=%u\n",
          nr, (unsigned)i2c->bus_speed_hz, (unsigned)i2c->ic_clk_hz,
          (unsigned)i2c->tx_fifo_depth, (unsigned)i2c->rx_fifo_depth);
    return 0;
}

static int dw_i2c_remove(device_t *dev)
{
    dw_i2c_controller_t *i2c = dev ? dev->drv_priv : NULL;
    if (!i2c)
        return 0;
    int ret = i2c_adapter_remove(&i2c->adapter);
    if (ret)
        return ret;
    (void)dw_i2c_disable(i2c);
    dev->drv_priv = NULL;
    kfree(i2c);
    return 0;
}

/* ---- register model + mock slave (boot self-test only) ------------------- *
 *
 * Derived from the behaviour Linux's i2c-designware expects: IC_TAR is only
 * latched when the core is enabled, DATA_CMD bits 8/9/10 select read/STOP/
 * RESTART, TFNF clears while the RX FIFO is full, TX_ABRT_SOURCE reports the
 * address NAK, and IC_ENABLE_STATUS mirrors IC_ENABLE.  A repeated start with
 * RESTART_EN clear is an arbitration-style abort, which makes a wrong IC_CON
 * observable.
 *
 * The slave side implements the I2C-HID register protocol: a write of two
 * bytes selects a register, a following read streams from it, and a bare read
 * (after STOP) is the device's input report.
 */

#define DW_SIM_FIFO 16U

typedef struct dw_i2c_slave {
    uint8_t addr;
    const uint8_t *hid_desc;
    uint16_t hid_desc_len;
    const uint8_t *report_desc;
    uint16_t report_desc_len;
    const uint8_t *input;
    uint16_t input_len;

    uint16_t pending_reg;
    uint16_t reg_cursor;
    uint16_t input_pos;
    uint8_t addr_phase;
    uint8_t have_reg;
    uint8_t last_cmd[8];
    uint8_t last_cmd_len;
} dw_i2c_slave_t;

typedef struct dw_i2c_sim {
    uint32_t reg[DW_SIM_REGS / 4U];
    uint32_t rxq[DW_SIM_FIFO];
    uint8_t rx_head, rx_tail, rx_count;
    uint32_t latched_tar;
    uint32_t tar;
    uint32_t abort_src;
    int stop_seen;
    int enabled;
    dw_i2c_slave_t *slave;
} dw_i2c_sim_t;

static uint8_t dw_sim_slave_read(dw_i2c_slave_t *s)
{
    if (!s->have_reg) {
        if (s->input && s->input_pos < s->input_len)
            return s->input[s->input_pos++];
        return 0;
    }
    const uint8_t *src = NULL;
    uint16_t len = 0;
    if (s->pending_reg == 0x0001 && s->hid_desc) {
        src = s->hid_desc;
        len = s->hid_desc_len;
    } else if (s->pending_reg == 0x0010 && s->report_desc) {
        src = s->report_desc;
        len = s->report_desc_len;
    }
    uint8_t b = (src && s->reg_cursor < len) ? src[s->reg_cursor] : 0;
    if (s->reg_cursor < 0xffffU)
        s->reg_cursor++;
    return b;
}

static void dw_sim_slave_write(dw_i2c_slave_t *s, uint8_t b)
{
    if (!s->have_reg) {
        if (s->addr_phase == 0) {
            s->pending_reg = b;
            s->addr_phase = 1;
        } else {
            s->pending_reg |= (uint16_t)b << 8;
            s->addr_phase = 2;
            s->have_reg = 1;
            s->reg_cursor = 0;
        }
        return;
    }
    if (s->last_cmd_len < sizeof(s->last_cmd))
        s->last_cmd[s->last_cmd_len++] = b;
}

static void dw_sim_slave_stop(dw_i2c_slave_t *s)
{
    s->have_reg = 0;
    s->addr_phase = 0;
    s->input_pos = 0;
}

static void dw_sim_data_cmd(dw_i2c_controller_t *i2c, uint32_t cmd)
{
    dw_i2c_sim_t *sim = i2c->sim;
    dw_i2c_slave_t *s = sim->slave;
    uint32_t con = sim->reg[DW_IC_CON >> 2];

    if (cmd & DW_CMD_RESTART) {
        if (!(con & DW_CON_RESTART_EN)) {
            sim->abort_src = DW_ABRT_ARB_LOST;
            return;
        }
        /* A repeated start keeps the slave's register pointer. */
    }
    if (!s)
        return;
    if (cmd & DW_CMD_READ) {
        if (sim->tar != s->addr) {
            sim->abort_src = DW_ABRT_7B_ADDR_NOACK;
            return;
        }
        if (sim->rx_count >= DW_SIM_FIFO) {
            sim->abort_src = DW_ABRT_ARB_LOST;
            return;
        }
        sim->rxq[sim->rx_tail] = dw_sim_slave_read(s);
        sim->rx_tail = (uint8_t)((sim->rx_tail + 1U) % DW_SIM_FIFO);
        sim->rx_count++;
    } else {
        if (sim->tar != s->addr) {
            sim->abort_src = DW_ABRT_7B_ADDR_NOACK;
            return;
        }
        dw_sim_slave_write(s, (uint8_t)(cmd & 0xffU));
    }
    if (cmd & DW_CMD_STOP) {
        sim->stop_seen = 1;
        dw_sim_slave_stop(s);
    }
}

static uint32_t dw_sim_read(dw_i2c_controller_t *i2c, uint32_t reg)
{
    dw_i2c_sim_t *sim = i2c->sim;
    switch (reg) {
        case DW_IC_COMP_TYPE:
            return DW_COMP_TYPE_VALUE;
        case DW_IC_COMP_PARAM_1:
            return (7U << 16) | (7U << 8); /* 8-entry FIFOs */
        case DW_IC_STATUS: {
            uint32_t st = DW_STATUS_TFE;
            if (sim->rx_count < 8U)
                st |= DW_STATUS_TFNF;
            if (sim->rx_count)
                st |= DW_STATUS_RFNE;
            return st;
        }
        case DW_IC_ENABLE_STATUS:
            return sim->enabled ? 1U : 0U;
        case DW_IC_ENABLE:
            return sim->enabled ? 1U : 0U;
        case DW_IC_RXFLR:
            return sim->rx_count;
        case DW_IC_TXFLR:
            return 0;
        case DW_IC_RAW_INTR_STAT:
            return (sim->abort_src ? DW_INTR_TX_ABRT : 0) |
                   (sim->stop_seen ? DW_INTR_STOP_DET : 0);
        case DW_IC_TX_ABRT_SOURCE:
            return sim->abort_src;
        case DW_IC_CLR_TX_ABRT:
            sim->abort_src = 0;
            return 0;
        case DW_IC_CLR_INTR:
            sim->abort_src = 0;
            /* Fall through to clear STOP as on the hardware. */
            __attribute__((fallthrough));
        case DW_IC_CLR_STOP_DET:
            sim->stop_seen = 0;
            return 0;
        case DW_IC_DATA_CMD: {
            if (!sim->rx_count)
                return 0;
            uint32_t v = sim->rxq[sim->rx_head];
            sim->rx_head = (uint8_t)((sim->rx_head + 1U) % DW_SIM_FIFO);
            sim->rx_count--;
            return v;
        }
        default:
            return sim->reg[(reg & (DW_SIM_REGS - 1U)) >> 2];
    }
}

static void dw_sim_write(dw_i2c_controller_t *i2c, uint32_t reg,
                         uint32_t value)
{
    dw_i2c_sim_t *sim = i2c->sim;
    switch (reg) {
        case DW_IC_ENABLE:
            sim->enabled = (value & 1U) != 0;
            if (value & DW_ENABLE_ABORT) {
                sim->abort_src = DW_ABRT_MASTER_DIS;
                sim->rx_count = sim->rx_head = sim->rx_tail = 0;
                if (sim->slave)
                    dw_sim_slave_stop(sim->slave);
            }
            /* IC_TAR latches at enable, as on cores without
             * I2C_DYNAMIC_TAR_UPDATE. */
            if (sim->enabled)
                sim->tar = sim->latched_tar;
            return;
        case DW_IC_TAR:
            sim->latched_tar = value & 0x3ffU;
            if (!sim->enabled)
                sim->tar = sim->latched_tar;
            return;
        case DW_IC_DATA_CMD:
            dw_sim_data_cmd(i2c, value);
            return;
        case DW_IC_CLR_TX_ABRT:
        case DW_IC_CLR_INTR:
            sim->abort_src = 0;
            return;
        default:
            sim->reg[(reg & (DW_SIM_REGS - 1U)) >> 2] = value;
            return;
    }
}

/* ---- self-test ----------------------------------------------------------- */

static const uint8_t dw_test_hid_desc[30] = {
    30, 0, 0x00, 0x01,             /* length, bcdVersion 1.00 */
    0x2c, 0x01, 0x10, 0x00,       /* report descriptor length/register */
    0x03, 0x00, 0x20, 0x00,       /* input register / max input length */
    0x04, 0x00, 0x00, 0x00,       /* output register / length */
    0x05, 0x00, 0x06, 0x00,       /* command register / data register */
    0x6b, 0x05, 0x78, 0xce,       /* vendor/product */
    0x00, 0x01,                   /* version */
    0, 0, 0, 0,
};

static uint8_t dw_test_report_desc[300];
static uint8_t dw_test_input[10] = {0x0a, 0x00, 0x01, 0x01, 0xe8,
                                    0x03, 0xd0, 0x07, 0x01, 0x00};

int a20_boot_selftest_i2c_dw(void)
{
    static dw_i2c_sim_t sim;
    static dw_i2c_slave_t slave;
    static dw_i2c_controller_t ctrl;
    static i2c_adapter_t adapter;
    static uint8_t rd[300];

    memset(&sim, 0, sizeof(sim));
    memset(&slave, 0, sizeof(slave));
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&adapter, 0, sizeof(adapter));
    for (unsigned i = 0; i < sizeof(dw_test_report_desc); i++)
        dw_test_report_desc[i] = (uint8_t)(i * 7U + 3U);

    slave.addr = 0x2c;
    slave.hid_desc = dw_test_hid_desc;
    slave.hid_desc_len = sizeof(dw_test_hid_desc);
    slave.report_desc = dw_test_report_desc;
    slave.report_desc_len = sizeof(dw_test_report_desc);
    slave.input = dw_test_input;
    slave.input_len = sizeof(dw_test_input);
    sim.slave = &slave;

    ctrl.sim = &sim;
    ctrl.slave = &slave;
    ctrl.ic_clk_hz = 100000000U;
    ctrl.bus_speed_hz = 400000U;
    ctrl.tx_fifo_depth = 8U;
    ctrl.rx_fifo_depth = 8U;
    mutex_init(&ctrl.lock);
    adapter.name = "dw-sim";
    adapter.nr = 0;
    adapter.transfer = dw_i2c_transfer;
    adapter.priv = &ctrl;

    if (dw_read(&ctrl, DW_IC_COMP_TYPE) != DW_COMP_TYPE_VALUE)
        return -1;
    if (dw_i2c_init_hw(&ctrl) < 0)
        return -1;
    uint32_t con = dw_read(&ctrl, DW_IC_CON);
    if (!(con & DW_CON_MASTER) || !(con & DW_CON_SLAVE_DISABLE) ||
        !(con & DW_CON_RESTART_EN))
        return -1;
    if ((con & 0x6U) != DW_CON_SPEED_FAST)
        return -1;

    /* Register read: HID descriptor at 0x0001 (write 2 bytes, repeated start,
     * read 30). */
    uint8_t reg1[2] = {0x01, 0x00};
    i2c_msg_t m1[2] = {
        {.addr = 0x2c, .flags = I2C_M_WRITE, .len = 2, .buf = reg1},
        {.addr = 0x2c, .flags = I2C_M_READ, .len = 30, .buf = rd},
    };
    if (dw_i2c_transfer(&adapter, m1, 2) != 0)
        return -2;
    if (memcmp(rd, dw_test_hid_desc, 30) != 0)
        return -3;

    /* Long read: 300 bytes, far past the 8-entry RX FIFO. */
    uint8_t reg2[2] = {0x10, 0x00};
    i2c_msg_t m2[2] = {
        {.addr = 0x2c, .flags = I2C_M_WRITE, .len = 2, .buf = reg2},
        {.addr = 0x2c, .flags = I2C_M_READ, .len = sizeof(rd), .buf = rd},
    };
    if (dw_i2c_transfer(&adapter, m2, 2) != 0)
        return -4;
    if (memcmp(rd, dw_test_report_desc, sizeof(dw_test_report_desc)) != 0)
        return -5;

    /* Command write: SET_POWER payload [state, opcode] to register 0x0005. */
    uint8_t cmd[4] = {0x05, 0x00, 0x00, 0x08};
    i2c_msg_t m3 = {.addr = 0x2c, .flags = I2C_M_WRITE, .len = 4, .buf = cmd};
    if (dw_i2c_transfer(&adapter, &m3, 1) != 0)
        return -6;
    if (slave.last_cmd_len != 2 || slave.last_cmd[0] != 0x00 ||
        slave.last_cmd[1] != 0x08)
        return -7;

    /* A wrong target address must surface as an address NAK (-ENXIO). */
    uint8_t reg3[2] = {0x01, 0x00};
    i2c_msg_t m4[2] = {
        {.addr = 0x2d, .flags = I2C_M_WRITE, .len = 2, .buf = reg3},
        {.addr = 0x2d, .flags = I2C_M_READ, .len = 4, .buf = rd},
    };
    if (dw_i2c_transfer(&adapter, m4, 2) != -ENXIO)
        return -8;

    return 0;
}

static const firmware_device_id_t dw_i2c_ids[] = {
    {.id = "AMDI0010"}, /* AMD I2C controller (DesignWare) */
    {.id = "AMDI0011"},
    {.id = "snps,designware-i2c"},
    {.id = "snps,dw-apb-i2c"},
    {.id = NULL},
};

static const driver_t dw_i2c_driver = {
    .name = "i2c-designware",
    .bus = &platform_bus,
    .firmware_ids = dw_i2c_ids,
    .probe = dw_i2c_probe,
    .remove = dw_i2c_remove,
};

DRIVER_REGISTER(dw_i2c_driver);
