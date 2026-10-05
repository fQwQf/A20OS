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
struct audio_dev_ops;
struct virtio_gpu_mem_entry;
struct virtio_gpu_box;

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
class_device_t *class_device_ref_for_device(const struct device *dev);
int class_device_call_begin(class_device_t *cdev);
void class_device_call_end(class_device_t *cdev);
int class_device_has_devnode(const class_device_t *cdev);
uint8_t class_device_dirent_type(const class_device_t *cdev);
const char *class_device_subsystem(uint32_t type);
void class_device_emit_uevents(const char *action);

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
/* Driver-level counters, distinct from the stack's own per-netif counts in
 * a20_lwip_netif_state_t.  The stack counts what lwIP was handed; these count
 * what the device actually moved, so the two diverging is what localizes a
 * loss to the window between the ring and the protocol stack.
 *
 * Deliberately not a function pointer in net_dev_ops_t: that vtable is shared
 * with loadable .a20drv modules, so appending a field makes the kernel read
 * one field past the end of any module built against an older header, and then
 * call it.  Appended fields therefore require bumping A20_DRIVER_ABI, which
 * drvmod_load() now rejects on (a20_driver_descriptor_sane()), so a stale
 * module fails to load instead of being misread.  A weak symbol is a separate
 * mechanism and remains absent rather than wrong.
 *
 * Known limit: a weak symbol only resolves when the driver is linked into the
 * kernel, which today means the embedded profile (virtio_net.c is in
 * EMBEDDED_DEVICE_DRIVER_SRCS).  Under DRIVER_DEPLOYMENT=generic the driver is
 * a .a20drv package and the kernel cannot call into it by name, so the net_dev
 * report omits the driver line rather than showing zeros.  Closing that needs
 * either a size-checked vtable registration or an ABI version in the module
 * descriptor that drvmod_load() validates -- neither is a driver-local change.
 *
 * The same rule is why send_sg() and caps() below are appended rather than
 * inserted, and why landing them required A20_DRIVER_ABI 1 -> 2: the field
 * order is now part of the module ABI.
 */
typedef struct net_dev_stats {
    uint64_t rx_packets;
    uint64_t rx_drops;
    uint64_t tx_packets;
    uint64_t tx_drops;
} net_dev_stats_t;

/*
 * One segment of a transmit buffer chain.  Same shape as POSIX struct iovec
 * and the kernel's own net_txiovec_t, but spelled out here so the driver HAL
 * has no dependency on either.
 */
typedef struct net_iovec {
    const uint8_t *base;
    size_t         len;
} net_iovec_t;

/*
 * Capability bits returned by net_dev_ops_t::caps().
 *
 * These answer "what did this device actually negotiate", not "what could it in
 * principle do": a driver sets a bit only after the corresponding feature was
 * agreed with the hardware and the driver implements the matching path.  A
 * consumer that acts on a bit it was handed without checking the matching
 * callback is relying on the driver having lied, which is exactly the failure
 * this split exists to make impossible.
 *
 * Why the two checksum bits exist but stay clear:
 *
 * The stack does have a carrier for this handshake -- lwIP 2.2.2 carries the
 * NETIF_CHECKSUM_GEN_* and NETIF_CHECKSUM_CHECK_* bits plus a
 * netif->chksum_flags field
 * (lwip netif.h:140-153, :340-342, :408-417) behind the
 * LWIP_CHECKSUM_CTRL_PER_NETIF switch, which defaults to 0
 * (opt.h:2371-2373).  What is missing is the wiring on the A20OS side of it,
 * and one gap that no switch closes:
 *
 *  - the TCP send path finalises the segment checksum with no
 *    IF__NETIF_CHECKSUM_ENABLED() around it at all (lwip tcp_out.c:1587-1596),
 *    so enabling the switch would offload UDP and not TCP;
 *  - lwIP's per-netif bit is not a per-frame bit, and this HAL's recv() returns
 *    a length and nothing else, so "this frame's checksum was verified" has no
 *    channel to travel in.  QEMU's virtio-net does not supply it either: it
 *    never sets VIRTIO_NET_HDR_F_DATA_VALID;
 *  - the outbound path rewrites L4 checksums incrementally in place for NAT
 *    (kernel/net/netfilter_nat.c), which is arithmetic on a value that would no
 *    longer be a complete checksum.
 *
 * Setting either bit under those conditions would leave lwIP verifying a
 * checksum the device never computed -- a silent drop of every packet, not a
 * missing feature.  The bits are defined now and are set only by a driver whose
 * stack can express the handshake end to end.  MRG_RXBUF is different: it is a
 * pure device-side receive property lwIP never sees, so a driver that negotiated
 * it can and should report it.  The full investigation, including what each
 * required change would be, is docs/net/checksum-offload.md.
 */
#define NET_DEV_CAP_TX_SG           (1u << 0) /* send_sg() consumes a segment list */
#define NET_DEV_CAP_TX_CSUM_OFFLOAD (1u << 1) /* needs_csum/csum_start in the vnet hdr */
#define NET_DEV_CAP_RX_CSUM_OFFLOAD (1u << 2) /* hdr carries a valid, verified checksum */
#define NET_DEV_CAP_MRG_RXBUF       (1u << 3) /* device may merge several RX buffers */

typedef struct net_dev_ops {
    int            (*open)(struct device *dev);
    int            (*stop)(struct device *dev);
    int            (*send)(struct device *dev, const void *pkt, size_t len);
    int            (*recv)(struct device *dev, void *buf, size_t maxlen);
    const uint8_t *(*mac)(struct device *dev);
    void           (*poll)(struct device *dev);
    int            (*ioctl)(struct device *dev, unsigned long req, void *arg);
    /* Optional carrier query. Drivers without PHY/link reporting retain the
     * historical always-up behavior. */
    int            (*link_up)(struct device *dev);
    /* Optional: reports whether RX arrival raises an IRQ, so a waiter may skip
     * polling when no device signalled work.  NULL means unknown, and unknown is
     * treated as "no IRQ" so an unconverted driver keeps being drained
     * unconditionally rather than silently losing RX. */
    int            (*rx_irq_driven)(struct device *dev);
    /* Optional scatter-gather transmit, appended so every existing
     * designated initializer keeps compiling.  Consumes nr segments whose total
     * length is the frame, and returns the frame length exactly as send() does,
     * or a negative errno.  NULL -- the state of every driver that predates
     * this field -- means "linear only": the stack copies the pbuf chain into
     * its staging buffer and calls send() instead, so an unconverted driver
     * sees byte-for-byte the same frames at the same points as before.
     *
     * The caller keeps ownership of every segment for the duration of the call.
     * A driver must not retain them. */
    int            (*send_sg)(struct device *dev, const net_iovec_t *iov,
                              unsigned nr);
    /* Optional capability query, in the same appended-field position and for
     * the same reason: see the ABI note above net_dev_stats_t.  NULL, or a 0
     * return, means "linear send only, no offload, no mergeable RX", which is
     * the safe reading for a driver that has not been converted. */
    uint32_t       (*caps)(struct device *dev);
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
    /* Copy up to cap bytes of the display's base EDID block into buf.
     * Returns the EDID length (>=128) or a negative errno when the device
     * has no EDID source. */
    int     (*get_edid)(struct device *dev, uint8_t *buf, size_t cap);
    /* 3D (virgl) transport.  These take kernel-built arguments, not user
     * pointers, so they cannot go through ioctl()'s copy_from_user path.
     * capset_buf/len fetch the host capability blob; attach_backing hands
     * the host physical pages backing a 3D resource.
     *
     * get_capset writes at most len bytes into buf and stores the count it
     * actually produced in *out_len.  The count is not a courtesy: callers
     * copy the blob straight to userspace, and a driver that fills less than
     * the buffer it was handed leaves the remainder as whatever the allocator
     * returned.  A capset can also legitimately be smaller than the caller's
     * buffer, so the caller cannot assume len bytes are valid either. */
    int     (*get_capset)(struct device *dev, uint32_t ctx_id, uint32_t index,
                          uint32_t version, void *buf, size_t len,
                          size_t *out_len);
    int     (*capset_info)(struct device *dev, uint32_t index,
                           uint32_t *id, uint32_t *max_version, uint32_t *max_size);
    /* Report what the device actually negotiated.  out_3d is non-zero only when
     * VIRTIO_GPU_F_VIRGL was agreed, out_context_init only when
     * VIRTIO_GPU_F_CONTEXT_INIT was.  The DRM layer has to answer
     * VIRTGPU_PARAM_3D_FEATURES before a client has done anything, so a
     * hardcoded 1 would send every 2D-only guest down the 3D path and report a
     * capability that was never negotiated. */
    int     (*get_features)(struct device *dev, uint32_t *out_3d,
                            uint32_t *out_context_init);
    int     (*resource_attach_backing)(struct device *dev, uint32_t resource_id,
                                       const struct virtio_gpu_mem_entry *entries,
                                       uint32_t nr_entries);
    int     (*ctx_create)(struct device *dev, uint32_t ctx_id, uint32_t context_init,
                          const char *name, size_t nlen);
    int     (*ctx_destroy)(struct device *dev, uint32_t ctx_id);
    int     (*resource_create_3d)(struct device *dev, uint32_t ctx_id,
                                  uint32_t resource_id, uint32_t target, uint32_t format,
                                  uint32_t bind, uint32_t width, uint32_t height,
                                  uint32_t depth, uint32_t array_size,
                                  uint32_t last_level, uint32_t nr_samples, uint32_t flags);
    int     (*resource_unref)(struct device *dev, uint32_t resource_id);
    /* Publish a resource to a context.  This is not implied by resource
     * creation: virglrenderer keeps its resources on a per-context list, and a
     * command stream that names a resource the context has never seen is
     * rejected as an illegal resource while the host still reports the submit
     * as successful.  Every 3D resource a context will reference must pass
     * through here first. */
    int     (*ctx_attach_resource)(struct device *dev, uint32_t ctx_id,
                                   uint32_t resource_id);
    /* Pull a rendered region of a 3D resource back into guest memory.  This is
     * the only way a guest observes the result of a submit: the host renders
     * into its own copy, so a guest that reads its own pages without asking for
     * a transfer reads pre-render contents no matter what the renderer did. */
    int     (*transfer_from_host_3d)(struct device *dev, uint32_t ctx_id,
                                     uint32_t resource_id,
                                     const struct virtio_gpu_box *box,
                                     uint32_t level, uint32_t stride,
                                     uint32_t layer_stride, uint64_t offset,
                                     const struct virtio_gpu_mem_entry *entries,
                                     uint32_t nr_entries);
    int     (*submit_3d)(struct device *dev, uint32_t ctx_id,
                         const void *cmdbuf, size_t len);
} gpu_dev_ops_t;

typedef struct audio_dev_ops audio_dev_ops_t;

#endif /* _DRIVER_CLASS_H */
