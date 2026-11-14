#ifndef _GPU_CORE_H
#define _GPU_CORE_H

#include "drivers/core/driver_core.h"
struct class_device;
struct gpu_dev_ops;

/* A 3D-capable display device is preferred as the /dev/fb0 and DRM backend;
 * see gpu_device_register() in gpu_core.c for the promotion rule. */
int gpu_device_register(device_t *dev);
void gpu_device_unregister(device_t *dev);
/* Selection updates belong to serialized probe/remove. The selection owns
 * device metadata, not a driver binding; 3D transport support is captured at
 * registration, never read from a departing module under the selection lock. */
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
void gpu_device_access_end(struct gpu_device_access *);
/* Legacy borrowed snapshot: caller must hold lifecycle coordination through
 * use. Does not pin binding/ops. New consumers use retained/scoped access. */
device_t *gpu_device_get_default(void);

#endif
