#ifndef _GPU_CORE_H
#define _GPU_CORE_H

#include "drivers/core/driver_core.h"
#include "drivers/gpu/capabilities.h"
struct class_device;
struct gpu_dev_ops;
struct vmo;
struct mm_mapping_owner;
struct gpu_device_access;

/* Probe-only, before hardware use: install a per-binding physical mapping
 * owner and fallible pre-consumer gate, selecting managed probe cleanup.
 * Use for borrowed physical get_fb apertures. Existing VMAs make teardown
 * return EBUSY until their last reference retires; new admission stays shut. */
int gpu_device_physical_mapping_init(device_t *);
/* Live scoped access required. Success owns one resource reference; pass it
 * to MM as borrowed, then mm_mapping_owner_put. No lifecycle mutex or active
 * call is retained by that reference. Failure clears out. */
int gpu_device_physical_mapping_get(const struct gpu_device_access *,
                                     struct mm_mapping_owner **);

/* Default scanout policy over native class publications. Only SCANOUT roles
 * are eligible; prefer RENDER, then managed over BOOT_SCANOUT, with stable ties.
 * Missing/invalid capability providers fail registration without changing it. */
int gpu_device_register(device_t *dev);
void gpu_device_unregister(device_t *dev);
/* Selection updates belong to serialized probe/remove. The selection owns
 * device metadata, not a driver binding. Native rendering capability is
 * captured at registration, never inferred from a backend-specific vtable. */
/* Task context. Owns a class reference, possibly already offline. Admission
 * requires access_begin/ops_begin. Caller puts it with class_device_put.
 * NULL includes probe before class publication. No lifecycle mutex is taken;
 * a retained publication never follows rebind/promotion to another binding. */
struct class_device *gpu_device_default_get(void);
/* One scoped call lease: do not copy or begin over a live access object.
 * Success owns a publication reference and active call; the caller may drop
 * its original reference. All fields describe one binding, valid until end.
 * Failure clears the output. Task context; no lifecycle mutation/wait that
 * needs this call to drain. It does not retain returned framebuffer memory. */
struct gpu_device_access {
    struct class_device *publication;
    device_t *device;
    const struct gpu_dev_ops *ops;
};
int gpu_device_access_begin(struct class_device *, struct gpu_device_access *);
int gpu_device_default_begin(struct gpu_device_access *);
/* Live access required. Copies a native capability snapshot; clears out on
 * failure. Missing provider -> EOPNOTSUPP; malformed flags/role dependencies
 * -> EINVAL; positive provider errors -> EIO. No new lease or storage pin. */
int gpu_device_capabilities_get(const struct gpu_device_access *, struct gpu_capabilities *);
void gpu_device_access_end(struct gpu_device_access *);
/* Owning snapshot of immutable canonical RAM. offset is page aligned; size
 * describes visible bytes, whose rounded page coverage fits memory. The
 * snapshot is independent of the device, binding and module after get. Do
 * not copy an owning snapshot or overwrite a live output; pair it with put. */
struct gpu_scanout {
    struct vmo *memory;
    uint64_t offset;
    size_t size;
};
/* Caller holds a live access; get takes the callback's owning reference.
 * Only the contiguous immutable RAM kind is accepted initially. Failure
 * clears out; no silent fallback from a present but failing provider. */
int gpu_device_scanout_get(const struct gpu_device_access *, struct gpu_scanout *);
void gpu_scanout_put(struct gpu_scanout *);
/* Legacy borrowed snapshot: caller must hold lifecycle coordination through
 * use. Does not pin binding/ops. New consumers use retained/scoped access. */
device_t *gpu_device_get_default(void);

#endif
