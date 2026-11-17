/* Whole production module; ports, delays and core lifecycle ordering modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define A20_DRIVER_DESCRIPTOR(...)
#define RES_IOPORT 1
#define DEV_CLASS_AUDIO 6
typedef struct { int held; } spinlock_t;
static unsigned irq_depth;
static void spin_init(spinlock_t *lock) { lock->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held && !irq_depth); lock->held = 1; irq_depth++; return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(lock->held && irq_depth == 1); lock->held = 0; irq_depth--;
}
static int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags) {
    if (lock->held) return 0;
    *flags = spin_lock_irqsave(lock); return 1;
}
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv, *context;
    int managed, claims, stopped, active;
    int (*stop)(void *);
    void (*release)(void *);
    resource_t res[2];
} device_t;
typedef struct { const char *id; } firmware_device_id_t;
/* UAPI */
/* AUDIO */
typedef struct {
    const char *name;
    const firmware_device_id_t *firmware_ids;
    void *bus;
    int (*probe)(device_t *), (*remove)(device_t *);
    const audio_dev_ops_t *class_ops;
    int class_type;
} driver_t;
static int platform_bus;
static void drv_log(const char *format, ...) { (void)format; }
static int drv_driver_register(void *driver) { assert(driver); return 0; }
static void *drv_alloc(size_t);
static void drv_free(void *);
static uint8_t drv_in8(uint16_t);
static void drv_out8(uint16_t, uint8_t);
static void drv_mdelay(unsigned);
static int device_probe_use_remove(device_t *dev) { dev->managed = 1; return 0; }
static int device_add_execution_action(device_t *, int (*)(void *), void (*)(void *), void *);
static int device_claim_resource(device_t *, const resource_t *);
static void *drv_device_get_resource(void *pointer, int type, int index) {
    device_t *dev = pointer; assert(type == RES_IOPORT && index >= 0 && index < 2);
    return &dev->res[index];
}
/* PRODUCTION */

static device_t dev;
static unsigned allocated, freed, releases, released_claims, reads, writes, delayed;
static uint8_t control, command, divisor_bytes[2];
static unsigned divisor_count;
static int fail_alloc, fail_action, fail_claim, deny_off, deny_on, dead;
static int close_during_delay, stop_during_delay, replace_during_delay;
static void *allocation;
static void *drv_alloc(size_t bytes) {
    if (fail_alloc) return NULL;
    assert(!allocation); allocation = malloc(bytes); assert(allocation); allocated++; return allocation;
}
static void drv_free(void *pointer) {
    assert(pointer == allocation && !dev.drv_priv && !dev.active && !dev.claims);
    assert(!dev.context || dev.stopped);
    free(pointer); allocation = NULL; freed++;
}
static uint8_t drv_in8(uint16_t port) {
    assert(port == 0x61 && dev.claims == 2 && dev.context); reads++;
    return dead ? 0xffU : control;
}
static void drv_out8(uint16_t port, uint8_t value) {
    assert(dev.claims == 2 && dev.context && dev.managed); writes++;
    if (dead) return;
    if (port == 0x61) {
        if (deny_off && !(value & 3)) return;
        if (deny_on && (value & 3) == 3) return;
        control = value;
    } else if (port == 0x43) {
        command = value; divisor_count = 0;
    } else {
        assert(port == 0x42 && divisor_count < 2); divisor_bytes[divisor_count++] = value;
    }
}
static int play(uint32_t frequency, uint32_t duration) {
    a20_audio_tone_t tone = {frequency, duration}; dev.active++;
    int ret = pc_speaker_driver.class_ops->tone(&dev, &tone); dev.active--; return ret;
}
static void drv_mdelay(unsigned ms) {
    assert(!irq_depth && dev.active && ms && ms <= 10); delayed += ms;
    if (close_during_delay) {
        close_during_delay = 0; assert(!dev.stop(dev.context)); dev.stopped = 1;
    }
    if (stop_during_delay) {
        stop_during_delay = 0; assert(!pc_speaker_driver.class_ops->stop(&dev));
    }
    if (replace_during_delay) {
        replace_during_delay = 0; assert(!play(2000, 0));
    }
}
static int device_add_execution_action(device_t *d, int (*stop)(void *),
                                        void (*release)(void *), void *context) {
    assert(d->managed && !d->claims && context == d->drv_priv);
    if (fail_action) return -ENOMEM;
    d->stop = stop; d->release = release; d->context = context; return 0;
}
static int device_claim_resource(device_t *d, const resource_t *resource) {
    assert(d->stop && d->context && resource == &d->res[d->claims]);
    if (fail_claim == d->claims + 1) return -EBUSY;
    d->claims++; return 0;
}
static void reset(void) {
    assert(allocated == freed && !allocation && !dev.context && !dev.drv_priv && !irq_depth);
    memset(&dev, 0, sizeof(dev)); dev.res[0] = (resource_t){0x42, 0x43};
    dev.res[1] = (resource_t){0x61, 0x61}; control = 0x90;
    releases = released_claims = reads = writes = delayed = divisor_count = command = 0;
    fail_alloc = fail_action = fail_claim = deny_off = deny_on = dead = 0;
    close_during_delay = stop_during_delay = replace_during_delay = 0;
}
/* Only model the core's already-tested stop -> class active-call drain ->
 * remove -> LIFO devres sequence. No free can bypass a failed phase. */
static int unbind(void) {
    int ret;
    if (dev.stop && !dev.stopped) {
        ret = dev.stop(dev.context); if (ret) return ret; dev.stopped = 1;
    }
    if (dev.active) return -EBUSY;
    ret = pc_speaker_driver.remove(&dev); if (ret) return ret;
    released_claims += dev.claims; dev.claims = 0;
    if (dev.release) { dev.release(dev.context); releases++; }
    dev.context = NULL; dev.stop = NULL; dev.release = NULL; return 0;
}
static void retained(void *saved) {
    assert(dev.context == saved && dev.drv_priv == saved && allocation == saved);
    assert(dev.claims == 2 && !releases && !released_claims);
    unsigned before = reads + writes;
    assert(play(1000, 10) == -ESHUTDOWN);
    assert(pc_speaker_driver.class_ops->stop(&dev) == -ESHUTDOWN && before == reads + writes);
}
int main(void) {
    assert(!DriverEntry());
    reset(); dev.res[0].start = UINT64_MAX;
    assert(pc_speaker_driver.probe(&dev) == -ENODEV && !reads && !writes && !allocation);
    reset(); dev.res[1].start = dev.res[1].end = 0x10061;
    assert(pc_speaker_driver.probe(&dev) == -ENODEV && !allocation);
    reset(); fail_alloc = 1;
    assert(pc_speaker_driver.probe(&dev) == -ENOMEM && !dev.drv_priv); assert(!unbind());
    reset(); fail_action = 1;
    assert(pc_speaker_driver.probe(&dev) == -ENOMEM && !dev.drv_priv && !allocation); assert(!unbind());
    reset(); fail_claim = 1;
    assert(pc_speaker_driver.probe(&dev) == -EBUSY && !reads && !writes); assert(!unbind() && !released_claims);
    reset(); fail_claim = 2;
    assert(pc_speaker_driver.probe(&dev) == -EBUSY && !reads && !writes); assert(!unbind() && released_claims == 1);
    reset(); control = 0x93; deny_off = 1;
    assert(pc_speaker_driver.probe(&dev) == -EIO);
    void *saved = dev.context; assert(unbind() == -EIO); retained(saved);
    deny_off = 0; assert(!unbind() && control == 0x90 && releases == 1 && released_claims == 2);
    reset(); assert(!pc_speaker_driver.probe(&dev));
    assert(pc_speaker_driver.remove(&dev) == -EBUSY);
    assert(!play(1000, 21) && delayed == 21 && control == 0x90 && command == 0xb6);
    assert(divisor_count == 2 && ((unsigned)divisor_bytes[0] | ((unsigned)divisor_bytes[1] << 8)) == 1193);
    assert(!play(20000, 0) && (control & 3) == 3); assert(!pc_speaker_stop(&dev));
    unsigned before = reads + writes;
    assert(play(0, 10) == -EINVAL && play(1000, 5001) == -EINVAL && before == reads + writes);
    a20_audio_tone_t tone = {1000, 0}; dev.active++;
    assert(pc_speaker_write(&dev, &tone, sizeof(tone)) == sizeof(tone)); dev.active--;
    assert(!unbind() && control == 0x90);
    /* A stop or newer tone cancels the old delay without stopping its successor. */
    reset(); assert(!pc_speaker_driver.probe(&dev)); stop_during_delay = 1;
    assert(!play(1000, 5000) && delayed == 10 && control == 0x90); assert(!unbind());
    reset(); assert(!pc_speaker_driver.probe(&dev)); replace_during_delay = 1;
    assert(!play(1000, 5000) && delayed == 10 && control == 0x93); assert(!unbind());
    /* Lifecycle stop succeeds while a class call still owns storage; release
     * waits for that call to observe cancellation and leave. */
    reset(); assert(!pc_speaker_driver.probe(&dev)); close_during_delay = 1;
    assert(play(1000, 5000) == -ESHUTDOWN && delayed == 10 && control == 0x90);
    assert(allocation && !releases && !unbind() && releases == 1);
    reset(); assert(!pc_speaker_driver.probe(&dev)); saved = dev.context;
    uint64_t flags = spin_lock_irqsave(&((pc_speaker_t *)saved)->lock);
    assert(unbind() == -EBUSY); spin_unlock_irqrestore(&((pc_speaker_t *)saved)->lock, flags); retained(saved);
    assert(!unbind());
    reset(); assert(!pc_speaker_driver.probe(&dev)); assert(!play(1000, 0)); saved = dev.context;
    deny_off = 1; assert(unbind() == -EIO); retained(saved);
    dead = 1; assert(unbind() == -ENODEV); retained(saved);
    dead = deny_off = 0; assert(!unbind() && releases == 1);
    reset(); assert(!pc_speaker_driver.probe(&dev)); deny_on = 1;
    assert(play(1000, 0) == -EIO); retained(dev.context); deny_on = 0; assert(!unbind());
    reset(); assert(!pc_speaker_driver.probe(&dev)); assert(!play(1000, 0)); deny_off = 1;
    assert(pc_speaker_stop(&dev) == -EIO); retained(dev.context); deny_off = 0; assert(!unbind());
    assert(allocated == freed); puts("PC_SPEAKER_LIFETIME_OK: claimed ports, checked stop, bounded cancellation and retained partial probe");
}
