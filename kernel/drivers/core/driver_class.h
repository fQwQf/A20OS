/*
 * A20OS Driver Model — Device Class Interfaces
 *
 * Each device class (block, net, char) defines an ops struct.
 * Drivers implement the ops for their class; subsystems consume
 * them through class_ops pointer in driver_t.
 */
#ifndef _DRIVER_CLASS_H
#define _DRIVER_CLASS_H

#include "core/types.h"
#include "core/refcount.h"
#include "core/lock.h"

struct device;

#define CLASS_DEVICE_NAME_MAX 32

/*
 * A class device is the stable publication object between a bound hardware
 * device and devfs/sysfs. The registry owns one reference while online; open
 * files may keep it alive after unplug, but new operations then return ENODEV.
 */
typedef struct class_device {
    struct device *dev;
    uint32_t class_type;
    uint32_t index;
    uint64_t devt;
    char name[CLASS_DEVICE_NAME_MAX];
    refcount_t refs;
    spinlock_t state_lock;
    volatile unsigned active_calls;
    volatile int online;
} class_device_t;

int class_device_publish(struct device *dev);
void class_device_unpublish(struct device *dev);
class_device_t *class_device_get_by_name(const char *name);
class_device_t *class_device_get_by_type(uint32_t class_type, unsigned index);
class_device_t *class_device_get_nth(unsigned index);
void class_device_get(class_device_t *cdev);
void class_device_put(class_device_t *cdev);
int class_device_call_begin(class_device_t *cdev);
void class_device_call_end(class_device_t *cdev);
int class_device_has_devnode(const class_device_t *cdev);
uint8_t class_device_dirent_type(const class_device_t *cdev);

/* ============================================================
 * Block device operations
 *
 * Used by: VFS / FAT32 / EXT4 / block_cache
 * ============================================================ */
typedef struct block_dev_ops {
    int     (*read)(struct device *dev, uint64_t lba, void *buf, size_t sectors);
    int     (*write)(struct device *dev, uint64_t lba, const void *buf, size_t sectors);
    int     (*flush)(struct device *dev);
    int     (*ioctl)(struct device *dev, unsigned long req, void *arg);
    uint64_t (*capacity)(struct device *dev);
    uint32_t (*sector_size)(struct device *dev);
} block_dev_ops_t;

/* block ioctl requests */
#define BLK_IOCTL_GET_CAPACITY   0x1001
#define BLK_IOCTL_GET_SECTOR_SZ  0x1002
#define BLK_IOCTL_SYNC           0x1003

/* ============================================================
 * Network device operations
 *
 * Used by: lwIP network stack / socket layer
 * ============================================================ */
typedef struct net_dev_ops {
    int            (*open)(struct device *dev);
    int            (*stop)(struct device *dev);
    int            (*send)(struct device *dev, const void *pkt, size_t len);
    int            (*recv)(struct device *dev, void *buf, size_t maxlen);
    const uint8_t *(*mac)(struct device *dev);
    void           (*poll)(struct device *dev);
    int            (*ioctl)(struct device *dev, unsigned long req, void *arg);
} net_dev_ops_t;

/* net ioctl requests */
#define NET_IOCTL_GET_MAC    0x2001
#define NET_IOCTL_SET_MAC    0x2002
#define NET_IOCTL_GET_MTU    0x2003
#define NET_IOCTL_GET_STATUS 0x2004

/* ============================================================
 * Character device operations
 *
 * Used by: devfs / tty / console
 * ============================================================ */
typedef struct char_dev_ops {
    int     (*read)(struct device *dev, void *buf, size_t count);
    int     (*write)(struct device *dev, const void *buf, size_t count);
    int     (*ioctl)(struct device *dev, unsigned long req, void *arg);
    int     (*poll)(struct device *dev, short events);
} char_dev_ops_t;


/* ============================================================
 * Input device operations
 *
 * Used by: input subsystem / evdev
 * ============================================================ */
typedef struct input_dev_ops {
    int     (*read)(struct device *dev, void *buf, size_t count);
    int     (*ioctl)(struct device *dev, unsigned long req, void *arg);
    int     (*poll)(struct device *dev, short events);
} input_dev_ops_t;

/* ============================================================
 * Display/GPU device operations
 *
 * Used by: framebuffer / graphics subsystem
 * ============================================================ */
typedef struct gpu_dev_ops {
    int     (*get_info)(struct device *dev, uint32_t *width, uint32_t *height, uint32_t *bpp);
    int     (*get_fb)(struct device *dev, uintptr_t *fb_paddr, size_t *fb_size);
    int     (*flush)(struct device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    int     (*ioctl)(struct device *dev, unsigned long req, void *arg);
} gpu_dev_ops_t;

/* PCM-oriented audio interface. Drivers own buffering and blocking policy;
 * devfs provides stable /dev/audioN publication and disconnect handling. */
typedef struct audio_dev_ops {
    int (*read)(struct device *dev, void *buf, size_t count);
    int (*write)(struct device *dev, const void *buf, size_t count);
    int (*ioctl)(struct device *dev, unsigned long req, void *arg);
    int (*poll)(struct device *dev, short events);
} audio_dev_ops_t;

#endif /* _DRIVER_CLASS_H */
