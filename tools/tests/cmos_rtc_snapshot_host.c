/* Production module; only port behavior, allocation/time seed/core modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define A20_DRIVER_DESCRIPTOR(...)
#define RES_IOPORT 1
#define DEV_CLASS_NONE 0
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv;
    resource_t res[2];
    unsigned res_count, claims;
} device_t;
typedef struct { const char *id; } firmware_device_id_t;
typedef struct {
    const char *name;
    const firmware_device_id_t *firmware_ids;
    void *bus;
    int (*probe)(device_t *), (*remove)(device_t *);
    void (*shutdown)(device_t *);
    int class_type;
} driver_t;
static int platform_bus;
static void drv_log(const char *, ...);
static int drv_driver_register(void *driver) { assert(driver); return 0; }
static void *drv_alloc(size_t);
static void drv_free(void *);
static uint8_t drv_in8(uint16_t);
static void drv_out8(uint16_t, uint8_t);
static void drv_udelay(unsigned);
static void timekeeping_wallclock_set_hw(uint64_t);
static resource_t *device_get_resource(device_t *dev, int type, int index) {
    assert(type == RES_IOPORT && index >= 0);
    return (unsigned)index < dev->res_count ? &dev->res[index] : NULL;
}
static int device_claim_resource(device_t *, const resource_t *);
/* REGISTERS */
/* PRODUCTION */
/* EPOCH_BASE */
/* EPOCH_ROLLOVER */
/* EPOCH_LEAP */

static device_t dev;
static uint8_t regs[128], index_value;
static unsigned reads, writes, index_reads, delayed, seeded, fallback_logs;
static unsigned allocated, freed, a_reads, sec_reads, b_reads;
static uint64_t adopted;
static int fail_alloc, fail_claim, dead, stuck_uip, unstable_seconds, unstable_mode;
static int rollover, uip_after_fields, change_between_samples;
static void *allocation;
static void drv_log(const char *format, ...) {
    if (strstr(format, "century byte")) fallback_logs++;
}
static void *drv_alloc(size_t bytes) {
    if (fail_alloc) return NULL;
    assert(!allocation); allocation = malloc(bytes); assert(allocation); allocated++; return allocation;
}
static void drv_free(void *pointer) {
    assert(pointer == allocation); free(pointer); allocation = NULL; freed++;
}
static void timekeeping_wallclock_set_hw(uint64_t epoch) { adopted = epoch; seeded++; }
static void drv_udelay(unsigned us) {
    assert(us == 10); delayed += us; assert(delayed <= 20000);
    if (change_between_samples) { change_between_samples = 0; regs[CMOS_RTC_SEC] = 0x57; }
}
static uint8_t drv_in8(uint16_t port) {
    assert(dev.claims == dev.res_count); reads++;
    if (port == 0x70) { index_reads++; return 0xff; } /* QEMU index read is open bus */
    assert(port == 0x71);
    if (dead) return 0xff;
    unsigned reg = index_value & 0x7f;
    if (reg == CMOS_RTC_SEC) {
        sec_reads++;
        if (unstable_seconds) return sec_reads & 1 ? 0x55 : 0x56;
    }
    if (reg == CMOS_RTC_REG_A) {
        a_reads++;
        if (stuck_uip || (uip_after_fields && a_reads == 2)) return 0xa6;
    }
    if (reg == CMOS_RTC_REG_B) {
        b_reads++;
        if (unstable_mode) return b_reads & 1 ? 2 : 6;
    }
    if (rollover && reg == CMOS_RTC_MIN) {
        rollover = 0;
        regs[CMOS_RTC_SEC] = regs[CMOS_RTC_MIN] = regs[CMOS_RTC_HOUR] = 0;
        regs[CMOS_RTC_MDAY] = regs[CMOS_RTC_MONTH] = 1; regs[CMOS_RTC_YEAR] = 0x27;
    }
    return regs[reg];
}
static void drv_out8(uint16_t port, uint8_t value) {
    assert(dev.claims == dev.res_count && port == 0x70); /* never write CMOS data/SET */
    assert(!(value & 0x80)); index_value = value; writes++;
}
static int device_claim_resource(device_t *d, const resource_t *resource) {
    assert(resource == &d->res[d->claims]);
    if (fail_claim == (int)d->claims + 1) return -EBUSY;
    d->claims++; return 0;
}
static void reset(void) {
    assert(allocated == freed && !allocation && !dev.drv_priv && !dev.claims);
    memset(&dev, 0, sizeof(dev)); memset(regs, 0, sizeof(regs));
    dev.res_count = 2; dev.res[0] = (resource_t){0x70, 0x70}; dev.res[1] = (resource_t){0x71, 0x71};
    regs[CMOS_RTC_REG_A] = 0x26; regs[CMOS_RTC_REG_B] = 2;
    regs[CMOS_RTC_SEC] = 0x56; regs[CMOS_RTC_MIN] = 0x34; regs[CMOS_RTC_HOUR] = 0x12;
    regs[CMOS_RTC_MDAY] = 0x17; regs[CMOS_RTC_MONTH] = 0x11;
    regs[CMOS_RTC_YEAR] = 0x26; regs[CMOS_RTC_CENTURY] = 0x20;
    reads = writes = index_reads = delayed = seeded = fallback_logs = a_reads = sec_reads = b_reads = 0;
    adopted = UINT64_MAX; index_value = 0;
    fail_alloc = fail_claim = dead = stuck_uip = unstable_seconds = unstable_mode = 0;
    rollover = uip_after_fields = change_between_samples = 0;
}
static void unbind(void) {
    assert(!cmos_rtc_driver.remove(&dev) && !dev.drv_priv);
    dev.claims = 0;
}
static void good(uint64_t expected) {
    assert(!cmos_rtc_driver.probe(&dev));
    cmos_rtc_t *rtc = dev.drv_priv;
    assert(rtc && rtc->have_time && rtc->epoch == expected && adopted == expected && seeded == 1);
    assert(!index_reads && !(index_value & 0x80)); unbind();
}
static void fallback(void) {
    assert(!cmos_rtc_driver.probe(&dev));
    cmos_rtc_t *rtc = dev.drv_priv;
    assert(rtc && !rtc->have_time && !seeded && adopted == UINT64_MAX);
    assert(!index_reads); unbind();
}
int main(void) {
    assert(!DriverEntry());
    reset(); dev.res[0].end = 0x10000;
    assert(cmos_rtc_driver.probe(&dev) == -ENODEV && !reads && !writes); unbind();
    reset(); dev.res[1].start = dev.res[1].end = 0x70;
    assert(cmos_rtc_driver.probe(&dev) == -ENODEV && !reads && !writes); unbind();
    reset(); fail_claim = 1;
    assert(cmos_rtc_driver.probe(&dev) == -EBUSY && !allocation && !reads); unbind();
    reset(); fail_claim = 2;
    assert(cmos_rtc_driver.probe(&dev) == -EBUSY && dev.claims == 1 && !allocation && !reads); unbind();
    reset(); fail_alloc = 1;
    assert(cmos_rtc_driver.probe(&dev) == -ENOMEM && dev.claims == 2 && !reads); unbind();
    reset(); good(EPOCH_BASE); assert(delayed == 10);
    reset(); dev.res_count = 1; dev.res[0].end = 0x71; good(EPOCH_BASE);
    reset(); regs[CMOS_RTC_REG_B] = 6;
    regs[CMOS_RTC_SEC] = 56; regs[CMOS_RTC_MIN] = 34; regs[CMOS_RTC_HOUR] = 12;
    regs[CMOS_RTC_MDAY] = 17; regs[CMOS_RTC_MONTH] = 11;
    regs[CMOS_RTC_YEAR] = 26; regs[CMOS_RTC_CENTURY] = 20; good(EPOCH_BASE);
    reset(); regs[CMOS_RTC_REG_B] = 0; regs[CMOS_RTC_HOUR] = 0x12; good(EPOCH_BASE - 43200);
    reset(); regs[CMOS_RTC_REG_B] = 0; regs[CMOS_RTC_HOUR] = 0x92; good(EPOCH_BASE);
    reset(); regs[CMOS_RTC_REG_B] = 0; regs[CMOS_RTC_HOUR] = 0; fallback();
    reset(); regs[CMOS_RTC_HOUR] = 0x92; fallback(); /* PM bit invalid in 24h */
    reset(); regs[CMOS_RTC_SEC] = 0; regs[CMOS_RTC_MIN] = 0; regs[CMOS_RTC_HOUR] = 0;
    regs[CMOS_RTC_MDAY] = 0x29; regs[CMOS_RTC_MONTH] = 2; regs[CMOS_RTC_YEAR] = 0;
    good(EPOCH_LEAP);
    reset(); regs[CMOS_RTC_MDAY] = 0x29; regs[CMOS_RTC_MONTH] = 2;
    regs[CMOS_RTC_YEAR] = 0; regs[CMOS_RTC_CENTURY] = 0x21; fallback(); /* 2100 is not leap */
    reset(); regs[CMOS_RTC_YEAR] = 0x60; regs[CMOS_RTC_CENTURY] = 0x19;
    fallback(); /* preserve pre-1970 rejection, never reinterpret as 2060 */
    const uint8_t bad_century[] = {0, 0xff, 0xfa, 0x99};
    for (unsigned i = 0; i < sizeof(bad_century); i++) {
        reset(); regs[CMOS_RTC_CENTURY] = bad_century[i]; good(EPOCH_BASE); assert(fallback_logs == 1);
    }
    reset(); regs[CMOS_RTC_REG_B] |= 0x80; good(EPOCH_BASE);
    reset(); regs[CMOS_RTC_YEAR] = 0xfa; fallback();
    reset(); rollover = 1; regs[CMOS_RTC_SEC] = regs[CMOS_RTC_MIN] = 0x59;
    regs[CMOS_RTC_HOUR] = 0x23; regs[CMOS_RTC_MDAY] = 0x31; regs[CMOS_RTC_MONTH] = 0x12;
    good(EPOCH_ROLLOVER); assert(delayed >= 20);
    reset(); uip_after_fields = 1; good(EPOCH_BASE); assert(delayed >= 20);
    reset(); change_between_samples = 1; good(EPOCH_BASE + 1); assert(delayed == 20);
    reset(); stuck_uip = 1; fallback(); assert(delayed == 20000);
    reset(); unstable_seconds = 1; fallback(); assert(delayed == 20000);
    reset(); unstable_mode = 1; fallback(); assert(delayed == 20000);
    reset(); dead = 1; fallback(); assert(!delayed);
    assert(allocated == freed); puts("CMOS_SNAPSHOT_OK: stable rollover-safe calendar, format fallback, NMI index policy and claimed ports");
}
