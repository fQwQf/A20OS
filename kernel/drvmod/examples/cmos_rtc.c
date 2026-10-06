/*
 * CMOS RTC (MC146818) — drvmod module (x86_64).
 *
 * The x86 wall clock source: the board files declare an `cmos-rtc` platform
 * device carrying the two fixed I/O ports (index 0x70, data 0x71), and this
 * driver binds to it through the same unified driver_t path every other
 * driver uses (drv_driver_register -> driver core -> platform_bus.match).
 * There is no second registration route and no private bus.
 *
 * Scope of this driver, deliberately narrow:
 *   - read-only: it never writes a CMOS register.  A driver that pokes 0x70/
 *     0x71 also owns the update-inhibit bit and therefore the machine's
 *     clock, which nothing in A20OS can yet write back through a class;
 *   - no IRQ.  The MC146818 update-complete interrupt is not wired up, so the
 *     wall clock is sampled once at bind time rather than tracked;
 *   - no time zone conversion.  The registers hold whatever the firmware/VM
 *     put there; `qemu-system-x86_64 -rtc base=utc` makes that UTC, and
 *     smoke-rtc-cmos pins that flag so the guest clock and the host clock are
 *     comparable.  A PC whose CMOS holds local time needs the UTC offset
 *     applied on top of this, which is not implemented (see
 *     docs/drivers/meta/implementation-status.md).
 *
 * On a successful read the driver hands the epoch to the kernel through
 * timekeeping_wallclock_set_hw(), which re-seeds the wall clock that
 * timekeeping_init() had to fall back to.  When the registers do not hold a
 * plausible time the driver still binds (the ports exist) and the kernel
 * keeps its build-time seed; both outcomes are logged.
 */

#include "drvmod/drvmod.h"

/* The identity constants are needed by the descriptor below, so this header
 * comes first (pc_spkr.c orders it the same way). */
#include "drivers/char/cmos_rtc.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_RTC, "cmos-rtc", A20_DRIVER_ABI,
                      A20_DRIVER_RES_IOPORT,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           A20_PLATFORM_VENDOR,
                                           A20_DEVICE_CMOS_RTC));

#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timekeeping.h"

/* UIP is asserted for the last 244us of the update cycle, so a couple of
 * milliseconds of polling is plenty; the bound is what keeps a wedged or
 * absent RTC from spinning forever in probe. */
#define CMOS_UIP_POLL_US    10U
#define CMOS_UIP_POLL_MAX   2000U /* 20ms */

/* Plausible window for a wall clock.  Wider than "after 1970" so a machine
 * with a dead battery (which lands on 2000-01-01) still seeds instead of
 * falling back, narrow enough that an all-ones CMOS bus does not. */
#define CMOS_TIME_MIN_YEAR  1970
#define CMOS_TIME_MAX_YEAR  2100

/* Default century when the century register cannot be trusted.  20 is not a
 * guess about the world, it is the only century a machine that booted into a
 * released OS in this century can be in; see cmos_rtc_resolve_year(). */
#define CMOS_DEFAULT_CENTURY 20

typedef struct {
    uint16_t index_port;
    uint16_t data_port;
    uint64_t epoch;        /* last successfully read wall clock */
    int      sec, min, hour, mday, month, year;
    uint8_t  binary;       /* register B DM bit: counters are not BCD */
    uint8_t  frozen;       /* register B SET bit: the chip is not counting */
    uint8_t  century_raw;  /* CMOS 0x32 exactly as read */
    uint8_t  have_time;
} cmos_rtc_t;

static void cmos_select(cmos_rtc_t *rtc, uint8_t reg)
{
    /* Bit 7 of the index register is NMI mask, not part of the register
     * number: writing the index without preserving it would unblock the NMI
     * the firmware may have deliberately masked. */
    uint8_t cur = drv_in8(rtc->index_port);
    drv_out8(rtc->index_port, (uint8_t)((cur & 0x80u) | (reg & 0x7fu)));
}

static uint8_t cmos_read(cmos_rtc_t *rtc, uint8_t reg)
{
    cmos_select(rtc, reg);
    return drv_in8(rtc->data_port);
}

/* Wait for the update cycle to finish before touching the time registers.
 * Returns -ETIMEDOUT rather than reading a half-updated calendar. */
static int cmos_wait_uip_clear(cmos_rtc_t *rtc)
{
    unsigned waited;
    for (waited = 0; waited < CMOS_UIP_POLL_MAX; waited += CMOS_UIP_POLL_US) {
        if (!(cmos_read(rtc, CMOS_RTC_REG_A) & CMOS_RTC_REGA_UIP))
            return 0;
        drv_udelay(CMOS_UIP_POLL_US);
    }
    return -ETIMEDOUT;
}

/* Register B's DM bit selects binary counters; 0 means BCD.  Both encodings
 * are decoded here because the mode is firmware state, not a device
 * property, and a driver that assumed either one reads nonsense on the
 * other.  A BCD byte with a nibble above 9 is not a clock value at all. */
static int cmos_decode(uint8_t raw, int binary)
{
    if (binary)
        return raw;
    if ((raw & 0x0fu) > 9u || (raw >> 4) > 9u)
        return -ERANGE;
    return (int)((raw >> 4) * 10u + (raw & 0x0fu));
}

static int cmos_is_leap(int year)
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static int cmos_days_in_month(int year, int month)
{
    static const int month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12)
        return -ERANGE;
    if (month == 2 && cmos_is_leap(year))
        return 29;
    return month_days[month - 1];
}

/*
 * CMOS 0x32 (century) is the one field this driver does not take at face
 * value.  It is written by the BIOS, not by the chip, and machines reach
 * userspace with it left at 0x00/0xff, holding a two-digit year from a
 * different century, or holding garbage that happens to look like BCD.
 * Trusting any of those silently produces a wall clock that is wrong by a
 * century -- a failure no later log line explains.  So: decode it, accept it
 * only when it yields a year in the plausible window, and otherwise keep the
 * two-digit year register under the default century.  That is a real
 * narrowing, not a workaround: a PC whose firmware left the century register
 * at 0x00 while the year register says 84 gets 2084 here, not 1984.
 *
 * Returns 0 when the register was used, -ERANGE when the default was taken
 * (the caller logs that; the time is still usable).
 */
static int cmos_rtc_resolve_year(int year2, int century, int *year_out)
{
    if (century >= 19 && century <= 21) {
        *year_out = century * 100 + year2;
        return 0;
    }
    *year_out = CMOS_DEFAULT_CENTURY * 100 + year2;
    return -ERANGE;
}

/* Days from 1970-01-01, the same Hinnant decomposition the devfs RTC ioctl
 * path uses, so both agree on every date they can represent. */
static int64_t cmos_days_from_civil(int year, int month, int mday)
{
    int era_y = year - (month <= 2);
    int era = (era_y >= 0 ? era_y : era_y - 399) / 400;
    unsigned yoe = (unsigned)(era_y - era * 400);
    unsigned doy = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2u) / 5u +
                   (unsigned)mday - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int cmos_rtc_read(cmos_rtc_t *rtc, uint64_t *epoch_out,
                         int *century_fallback)
{
    uint8_t reg_a, reg_b, hour_raw, century_raw;
    int binary, hour24, century;
    int year2;

    *century_fallback = 0;
    reg_a = cmos_read(rtc, CMOS_RTC_REG_A);
    reg_b = cmos_read(rtc, CMOS_RTC_REG_B);
    if (reg_a == 0xff && reg_b == 0xff)
        return -ENODEV; /* no CMOS behind these ports: an open bus reads all ones */

    /* Register A carries no data-format or hour-format bit on this part --
 * both live in register B -- so these two registers are all the mode the
 * driver needs. */
    hour24 = (reg_b & CMOS_RTC_REGB_24HOUR) != 0;
    binary = (reg_b & CMOS_RTC_REGB_BINARY) != 0;

    if (cmos_wait_uip_clear(rtc) < 0)
        return -ETIMEDOUT;

    hour_raw = cmos_read(rtc, CMOS_RTC_HOUR);
    century_raw = cmos_read(rtc, CMOS_RTC_CENTURY);
    rtc->century_raw = century_raw;

    rtc->sec   = cmos_decode(cmos_read(rtc, CMOS_RTC_SEC), binary);
    rtc->min   = cmos_decode(cmos_read(rtc, CMOS_RTC_MIN), binary);
    rtc->mday  = cmos_decode(cmos_read(rtc, CMOS_RTC_MDAY), binary);
    rtc->month = cmos_decode(cmos_read(rtc, CMOS_RTC_MONTH), binary);
    rtc->hour  = cmos_decode(hour_raw & 0x7fu, binary);
    year2      = cmos_decode(cmos_read(rtc, CMOS_RTC_YEAR), binary);
    /* An all-ones century register is the classic "never programmed" marker;
     * treat it as absent rather than as BCD 171. */
    century = (century_raw == 0xff) ? 0 : cmos_decode(century_raw, binary);

    if (!hour24) {
        if (rtc->hour < 0 || rtc->hour > 12)
            return -ERANGE;
        if (rtc->hour == 12)
            rtc->hour = 0;
        if (hour_raw & CMOS_RTC_HOUR_PM)
            rtc->hour += 12;
    }

    if (rtc->sec < 0 || rtc->min < 0 || rtc->hour < 0 || rtc->mday < 0 ||
        rtc->month < 0 || year2 < 0 || year2 > 99)
        return -ERANGE;
    if (century == 0)
        century = CMOS_DEFAULT_CENTURY;
    if (century < 0)
        return -ERANGE;

    int assumed = cmos_rtc_resolve_year(year2, century, &rtc->year);
    rtc->binary = (uint8_t)binary;
    /* SET means the update cycle is inhibited, so these registers hold the
     * last programmed time rather than a running one.  The read is still
     * plausible, but the caller deserves to know it is not advancing. */
    rtc->frozen = (uint8_t)((reg_b & CMOS_RTC_REGB_SET) != 0);

    if (rtc->sec > 59 || rtc->min > 59 || rtc->hour > 23)
        return -ERANGE;
    if (rtc->year < CMOS_TIME_MIN_YEAR || rtc->year > CMOS_TIME_MAX_YEAR)
        return -ERANGE;
    int mdays = cmos_days_in_month(rtc->year, rtc->month);
    if (mdays < 0 || rtc->mday < 1 || rtc->mday > mdays)
        return -ERANGE;

    int64_t days = cmos_days_from_civil(rtc->year, rtc->month, rtc->mday);
    *epoch_out = (uint64_t)days * 86400ULL + (uint64_t)rtc->hour * 3600ULL +
                 (uint64_t)rtc->min * 60ULL + (uint64_t)rtc->sec;
    /* Success even when the century had to be assumed: the caller reports
     * that through *century_fallback rather than through the return value,
     * because a negative return here means "no usable time" and nothing else
     * -- returning -ERANGE for both would let a rejected calendar reach the
     * wall clock as if it had been read. */
    *century_fallback = assumed;
    return 0;
}

static void cmos_rtc_log_ok(const cmos_rtc_t *rtc, uint64_t epoch, int fallback)
{
    drv_log("[CMOS-RTC] wall clock: %04d-%02d-%02d %02d:%02d:%02d %s "
            "epoch=%llu%s\n", rtc->year, rtc->month, rtc->mday, rtc->hour,
            rtc->min, rtc->sec, rtc->binary ? "(binary)" : "(bcd)",
            (unsigned long long)epoch,
            rtc->frozen ? " (register B SET: clock not advancing)" : "");
    if (fallback)
        drv_log("[CMOS-RTC] century byte (CMOS 0x%02x) unusable, assumed %d; "
                "if this is the wrong century the wall clock is wrong by 100 "
                "years\n", rtc->century_raw, CMOS_DEFAULT_CENTURY);
}

static int cmos_rtc_probe(device_t *dev)
{
    resource_t *index_res = device_get_resource(dev, RES_IOPORT, 0);
    resource_t *data_res = device_get_resource(dev, RES_IOPORT, 1);
    if (!index_res || !data_res || index_res->end < index_res->start ||
        data_res->end < data_res->start)
        return -ENODEV;

    cmos_rtc_t *rtc = (cmos_rtc_t *)drv_alloc(sizeof(*rtc));
    if (!rtc)
        return -ENOMEM;
    memset(rtc, 0, sizeof(*rtc));
    rtc->index_port = (uint16_t)index_res->start;
    rtc->data_port = (uint16_t)data_res->start;

    uint64_t epoch = 0;
    int century_fallback = 0;
    int ret = cmos_rtc_read(rtc, &epoch, &century_fallback);
    if (ret == 0) {
        rtc->have_time = 1;
        rtc->epoch = epoch;
        cmos_rtc_log_ok(rtc, epoch, century_fallback);
        /* Re-seed the kernel wall clock, which timekeeping_init() could only
         * approximate from the build timestamp: this module is activated from
         * the early DriverStore, long after timekeeping_init(). */
        timekeeping_wallclock_set_hw(epoch);
    } else {
        drv_log("[CMOS-RTC] no usable time (rc=%d); wall clock keeps the "
                "build-time seed\n", ret);
    }

    /* Bind even without a usable time: the ports are there, and refusing to
     * bind would hide the reason (which is logged) behind an absent device. */
    dev->drv_priv = rtc;
    return 0;
}

static int cmos_rtc_remove(device_t *dev)
{
    cmos_rtc_t *rtc = dev ? dev->drv_priv : NULL;
    if (rtc) {
        /* Read-only driver: the CMOS keeps counting, there is nothing to stop
         * and nothing mapped; only the instance state is dropped. */
        memset(rtc, 0, sizeof(*rtc));
        drv_free(rtc);
        dev->drv_priv = NULL;
    }
    return 0;
}

static const device_id_t cmos_rtc_ids[] = {
    { .vendor = A20_PLATFORM_VENDOR, .device = A20_DEVICE_CMOS_RTC },
    { 0 },
};

static driver_t cmos_rtc_driver = {
    .name = "cmos-rtc",
    .id_table = cmos_rtc_ids,
    .bus = &platform_bus,
    .probe = cmos_rtc_probe,
    .remove = cmos_rtc_remove,
    /* No RTC class ops exist in the unified model yet (driver_class.h defines
     * char/block/net/input/display/audio only), so this binds as a platform
     * device with no class device, exactly like the goldfish RTC module.
     * Consumers reach the time through timekeeping_get_realtime(), i.e.
     * /dev/rtc and /proc/stat, not through a class vtable. */
    .class_type = DEV_CLASS_NONE,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&cmos_rtc_driver);
    drv_log("[CMOS-RTC] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}