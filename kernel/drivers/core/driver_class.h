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

struct device;

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

#endif /* _DRIVER_CLASS_H */
