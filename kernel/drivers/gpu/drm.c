#include "drivers/gpu/drm.h"
#include "drivers/char/fbcon.h"
#include "drivers/gpu/drm_geom.h"
#include "drivers/gpu/drm_scanout.h"
#include "drivers/gpu/drm_scanout_mapping.h"
#include "drivers/gpu/virtio_gpu.h"

#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/poll.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/timer.h"
#include "core/timekeeping.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/workqueue.h"
#include "drivers/core/runtime_test.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/gpu/gpu_core.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "fs/sync_file.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/vmo.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * Minimal DRM/KMS backend.
 *
 * Each vfile captures one immutable DISPLAY publication. Compatibility GEM
 * handles and KMS state are shared within that publication; other bindings
 * have independent namespaces. KMS describes one scanout per publication.
 * Buffers use native VMOs, with storage ownership independent of class calls.
 */

#define DRM_GEM_BUCKETS 256
#define DRM_MAX_FBS 64
#define DRM_MAX_GEM_NAMES 64
#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_CTX_EVENT_MAX 16

/*
 * The single buffer abstraction.  A GEM object is a VMO plus the metadata
 * clients need to interpret it.  Dumb buffers are GEM objects that also carry
 * a linear layout (pitch/bpp); virgl resources are GEM objects the host
 * renders into.  Keeping one type means mmap, PRIME, ADDFB2 and the future
 * virgl path all operate on the same object, as in Linux.
 */
typedef struct drm_device drm_device_t;

typedef struct drm_gem {
    drm_device_t *device;
    struct drm_gem *next;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* 0 when the object has no linear layout */
    uint32_t bpp;        /* 0 when the object has no linear layout */
    uint32_t format;     /* DRM_FORMAT_* (drm_fourcc) */
    uint32_t usage;      /* DRM_BO_USE_* */
    struct vmo *vmo;
    uint64_t size;
    int is_virgl;        /* a host-side virgl resource mirrors this object */
    int backing_attached;/* host already has this object's physical pages */
    /* Which host context this resource was published to, 0 for none.  Tracked
     * per context rather than as a flag because one buffer is routinely used
     * from two opens -- wlroots allocates on one fd and draws on another -- and
     * each open owns a separate virgl context, so attaching to one says
     * nothing about the other. */
    uint32_t ctx_attached_for;
    uint32_t virgl_res_id;
    int fb_refs;         /* live framebuffers referencing this GEM */
    int dumb_live;       /* userspace still holds the dumb-buffer handle */
    /* Temporary operations and exported dma-bufs retain one pin each. Pins
     * keep CPU storage alive after the last handle/framebuffer or binding;
     * they do not authorize hardware access after the publication goes offline. */
    int pins;
} drm_gem_t;

/* A framebuffer is its own object, not an alias of the GEM handle.  Linux lets
 * userspace destroy the dumb buffer right after ADDFB2, so the fb must keep the
 * backing storage alive independently. */
typedef struct drm_fb {
    int used;
    uint32_t fb_id;
    uint32_t gem_handle;
} drm_fb_t;

typedef struct drm_context {
    struct drm_context *retained_next;
    drm_device_t *device;
    uint32_t magic;
    int is_master;
    int render_only;   /* opened via /dev/dri/renderD128: no master, no KMS */
    /* Lazily created host virgl context for this open.  Linux models a
     * virtgpu context as a property of the open file, not of the device,
     * and the resource-create/execbuffer ioctls take no context argument. */
    uint32_t virtgpu_ctx_id;
    int virtgpu_ctx_created;
    /* Capset this open selected through CONTEXT_INIT, resolved back to a
     * capset index for GET_CAPS.  0 means "unset": the host's first capset
     * is used, which is the pre-CONTEXT_INIT behaviour. */
    uint32_t virtgpu_capset_id;
    uint32_t virtgpu_capset_version;
    /* FIFO of completed DRM events (fixed 32-byte drm_event_vblank records)
     * destined for this open file.  Linux never overwrites a queued event,
     * so neither do we: wlroots matches page-flip completions to pending
     * flips and a lost event wedges its frame scheduler for good. */
    uint8_t events[DRM_CTX_EVENT_MAX][32];
    unsigned ev_head;
    unsigned ev_tail;
} drm_context_t;

/* A non-owning pointer, cleared under this lock before its file is freed.
 * Serialize the actual scanout copy against restoring the console.  Merely
 * closing a render/probe fd must not take the display from another client. */
static mutex_t g_scanout_owner_lock = MUTEX_INIT;
static drm_context_t *g_scanout_owner;

static void drm_console_release(drm_context_t *ctx)
{
    mutex_lock(&g_scanout_owner_lock);
    if (g_scanout_owner == ctx) {
        g_scanout_owner = NULL;
        fbcon_resume();
        kinfo("[DISPLAY] Desktop released display; console restored.\n");
    }
    mutex_unlock(&g_scanout_owner_lock);
}

/* A DRM namespace belongs to one immutable DISPLAY publication. This is
 * subsystem state over class publications, not another device publisher. */
struct drm_device {
    struct drm_device *next;
    class_device_t *publication;
    unsigned files, operations;
    mutex_t store_lock, gpu_lock;
    wait_queue_t gpu_waiters;
    drm_gem_t *gems[DRM_GEM_BUCKETS];
    drm_gem_t *retained;
    drm_context_t *retained_contexts;
    uint32_t gem_next_handle, fb_next_id, virtgpu_next_res;
    drm_fb_t fbs[DRM_MAX_FBS];
    struct {
        uint32_t fb_id, x, y, connector_ids[1], count_connectors;
    } crtc;
    struct {uint32_t name, handle;} gem_names[DRM_MAX_GEM_NAMES];
    int gem_name_count;
    uint8_t edid[128];
    int edid_ready;
    struct {
        mutex_t lock;
        wait_queue_t waiters;
        int flip_pending;
        drm_context_t *flip_ctx;
        uint64_t flip_user_data;
        uint32_t flip_crtc_id, sequence;
    } vblank;
};

typedef struct drm_call {
    drm_device_t *device;
    drm_context_t *context;
    struct gpu_device_access access; /* owning, stack scoped; never cached */
} drm_call_t;

static mutex_t g_drm_devices_lock = MUTEX_INIT;
static drm_device_t *g_drm_devices;
static struct driver_workqueue *g_drm_reaper_queue;
static struct driver_work *g_drm_reaper_work;
static void drm_reap(void *unused);

static int drm_reaper_init(void)
{
    if (g_drm_reaper_work) return 0;
    if (g_drm_reaper_queue) {
        int result = driver_workqueue_destroy(&g_drm_reaper_queue, 0);
        if (result) return result;
    }
    g_drm_reaper_queue = driver_workqueue_create();
    if (!g_drm_reaper_queue) return -ENOMEM;
    g_drm_reaper_work = driver_work_create(g_drm_reaper_queue, drm_reap, NULL);
    if (!g_drm_reaper_work) {
        /* If destroy fails, retain the static owner for initialization retry. */
        (void)driver_workqueue_destroy(&g_drm_reaper_queue, 0);
        return -ENOMEM;
    }
    return 0;
}
#define drm_lock(call) mutex_lock(&(call)->device->store_lock)
#define drm_unlock(call) mutex_unlock(&(call)->device->store_lock)

static void drm_device_offline(class_device_t *publication, void *data)
{
    (void)publication;
    drm_device_t *device = data;
    wait_queue_wake_all(&device->vblank.waiters, 0, PROC_WAKE_EVENT);
    wait_queue_wake_all(&device->gpu_waiters, 0, PROC_WAKE_EVENT);
    (void)driver_work_reschedule(g_drm_reaper_work, 0);
}

static int drm_device_online(drm_device_t *device)
{
    return __atomic_load_n(&device->publication->online, __ATOMIC_ACQUIRE);
}

/* The registry retains a namespace while legacy shared handles or failed
 * hardware cleanup may still own storage. Teardown retirement is separate
 * from class offline: never infer DMA stop from the absence of a binding. */
static int drm_device_get(class_device_t *publication, drm_device_t **out)
{
    *out = NULL;
    mutex_lock(&g_drm_devices_lock);
    int ready = drm_reaper_init();
    if (ready) { mutex_unlock(&g_drm_devices_lock); return ready; }
    for (drm_device_t *device = g_drm_devices; device; device = device->next) {
        if (device->publication != publication) continue;
        device->files++;
        *out = device;
        mutex_unlock(&g_drm_devices_lock);
        return 0;
    }
    drm_device_t *device = kcalloc(1, sizeof(*device));
    if (!device) { mutex_unlock(&g_drm_devices_lock); return -ENOMEM; }
    class_device_get(publication);
    device->publication = publication;
    device->files = 1;
    device->gem_next_handle = device->fb_next_id = device->virtgpu_next_res = 1;
    mutex_init(&device->store_lock);mutex_init(&device->gpu_lock);
    mutex_init(&device->vblank.lock);
    wait_queue_init(&device->gpu_waiters);wait_queue_init(&device->vblank.waiters);
    int result = class_device_set_offline(publication, drm_device_offline, device);
    if (result) {
        class_device_put(publication);kfree(device);
        mutex_unlock(&g_drm_devices_lock);
        return result;
    }
    device->next = g_drm_devices;g_drm_devices = device;
    *out = device;
    mutex_unlock(&g_drm_devices_lock);
    return 0;
}

static void drm_device_put(drm_device_t *device)
{
    mutex_lock(&g_drm_devices_lock);
    if (device->files) device->files--;
    mutex_unlock(&g_drm_devices_lock);
    (void)driver_work_reschedule(g_drm_reaper_work, 0);
}

static int drm_call_begin(drm_device_t *device, drm_context_t *context,
                           drm_call_t *call, bool require_online)
{
    *call = (drm_call_t){.device = device, .context = context};
    mutex_lock(&g_drm_devices_lock);
    device->operations++;
    mutex_unlock(&g_drm_devices_lock);
    /* Admission follows the device mutex. Waiting ioctls do not hold class
     * active calls that teardown would have to drain before waking them. */
    mutex_lock(&device->gpu_lock);
    int result = gpu_device_access_begin(device->publication, &call->access);
    if (result && require_online) {
        mutex_unlock(&device->gpu_lock);
        mutex_lock(&g_drm_devices_lock);
        device->operations--;
        mutex_unlock(&g_drm_devices_lock);
        (void)driver_work_reschedule(g_drm_reaper_work, 0);
        return result;
    }
    return 0;
}

static void drm_call_end(drm_call_t *call)
{
    gpu_device_access_end(&call->access);
    bool reap = call->device->retained || call->device->retained_contexts ||
                !drm_device_online(call->device);
    mutex_unlock(&call->device->gpu_lock);
    wait_queue_wake_all(&call->device->gpu_waiters, 0, PROC_WAKE_EVENT);
    mutex_lock(&g_drm_devices_lock);
    call->device->operations--;
    mutex_unlock(&g_drm_devices_lock);
    if (reap) (void)driver_work_reschedule(g_drm_reaper_work, clock_get_ticks() + MS_TO_TICKS(100));
}

static struct device *drm_gpu_device(const drm_call_t *call)
{
    return call->access.device;
}

static const gpu_dev_ops_t *drm_gpu_ops(const drm_call_t *call)
{
    return call->access.ops;
}


static int drm_scanout_read_vmo(void *opaque, uint64_t offset, void *out, size_t bytes)
{
    drm_gem_t *b = opaque;
    if (offset > b->size || bytes > b->size - offset)
        return -EINVAL;
    uint8_t *dst = out;
    while (bytes) {
        size_t in_page = offset & (PAGE_SIZE - 1);
        size_t count = PAGE_SIZE - in_page;
        if (count > bytes) count = bytes;
        pfn_t pfn = vmo_peek_page(b->vmo, (uint32_t)(offset / PAGE_SIZE));
        if (pfn == PFN_NONE) memset(dst, 0, count);
        else memcpy(dst, (uint8_t *)pfn_to_virt(pfn) + in_page, count);
        dst += count; offset += count; bytes -= count;
    }
    return 0;
}

/* KMS buffers and the device scanout are separate allocations. Copy and
 * convert dumb buffers into the driver's actual pitch and pixel layout before
 * flush; firmware GOP and DT scanouts need this just as virtual GPUs do. */
static int drm_present_buffer_at_locked(drm_call_t *call, drm_gem_t *b,
                                        uint32_t x, uint32_t y)
{
    drm_context_t *ctx = call->context;
    static unsigned int present_count;
    if (!b || !b->vmo)
        return -EINVAL;

    struct device *dev = drm_gpu_device(call);
    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!dev || !ops || !ops->get_info || !ops->get_fb || !ops->flush)
        return -ENODEV;

    uint32_t width = 0, height = 0, bpp = 0;
    uintptr_t fb_phys = 0;
    size_t fb_size = 0;
    if (ops->get_info(dev, &width, &height, &bpp) < 0 ||
        ops->get_fb(dev, &fb_phys, &fb_size) < 0)
        return -EINVAL;
    if (b->is_virgl) {
        if (!ops->scanout_resource)
            return -ENODEV;
        if (x >= b->width || y >= b->height ||
            width > b->width - x || height > b->height - y)
            return -EINVAL;
        return ops->scanout_resource(dev, b->virgl_res_id, x, y, width, height);
    }
    if (b->bpp != 32)
        return -EINVAL;
    gpu_scanout_layout_t layout = {
        .stride = width * 4, .red_mask = 0xff0000,
        .green_mask = 0xff00, .blue_mask = 0xff,
    };
    if (ops->get_layout) {
        int rc = ops->get_layout(dev, &layout);
        if (rc) return rc;
    } else if (bpp != 32 || width > (uint32_t)-1 / 4) {
        return -EINVAL;
    }
    drm_scanout_mapping_t mapped;
    int rc = drm_scanout_acquire(fb_phys, fb_size, &mapped);
    if (rc) return rc;
    drm_scanout_t scanout = {
        .width = width, .height = height, .pitch = layout.stride, .bpp = bpp,
        .red = layout.red_mask, .green = layout.green_mask, .blue = layout.blue_mask,
        .bytes = fb_size, .pixels = mapped.address,
    };
    uint32_t cols, rows;
    rc = drm_scanout_blit(&scanout, b->width, b->height, b->pitch, b->size,
                              x, y, drm_scanout_read_vmo, b, &cols, &rows);
    if (rc) {
        drm_scanout_release(&mapped);
        return rc;
    }

    /* The compositor has produced its first frame: from here the scanout is
     * userspace's, and kernel/tty text must not be drawn over it. */
    g_scanout_owner = ctx;
    fbcon_suspend();
    /* Flush only the rectangle that was written.  The rest of the host's copy
     * still holds whatever the previous present left there, which is what a
     * partial present means. */
    int ret = ops->flush(dev, x, y, cols, rows);
    drm_scanout_release(&mapped);
    if (present_count < 4) {
        kinfo("[DRM] present handle=%u %ux%u at %u,%u pages=%lu flush=%d\n",
              b->handle, cols, rows, x, y,
              (unsigned long)((b->size + PAGE_SIZE - 1) / PAGE_SIZE), ret);
        present_count++;
    }
    return ret;
}

static int drm_present_buffer_at(drm_call_t *call, drm_gem_t *b,
                                  uint32_t x, uint32_t y)
{
    mutex_lock(&g_scanout_owner_lock);
    int ret = drm_present_buffer_at_locked(call, b, x, y);
    mutex_unlock(&g_scanout_owner_lock);
    return ret;
}

/* ---- DRM wire structs (Linux ABI) ---- */

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    uint64_t name_len;
    char *name;
    uint64_t date_len;
    char *date;
    uint64_t desc_len;
    char *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_create {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t bpp;
    uint32_t size;
    uint32_t handle;
};

struct drm_gem_mmap {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

struct drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
};

struct drm_auth {
    uint32_t magic;
};

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_crtc_gamma {
    uint16_t red;
    uint16_t green;
    uint16_t blue;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_get_blob {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t connector_id;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

/* ---- virtio-gpu 3D wire structs (include/uapi/drm/virtgpu_drm.h) ---- */

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
};

struct drm_virtgpu_resource_create {
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t stride;
};

struct drm_virtgpu_resource_info {
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t blob_mem;
};

struct drm_virtgpu_3d_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
};

struct drm_virtgpu_3d_transfer {
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
};

struct drm_virtgpu_3d_wait {
    uint32_t handle;
    uint32_t flags;
};

struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct drm_event_vblank {
    uint32_t type;
    uint32_t length;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

/* Move a pending flip into its owner's event FIFO once the present has
 * completed (which, on this hardware, is before the ioctl returns).
 * Caller holds call->device->vblank.lock. */
static void drm_vblank_deliver_due_locked(drm_call_t *call)
{
    if (!call->device->vblank.flip_pending)
        return;

    drm_context_t *ctx = call->device->vblank.flip_ctx;
    unsigned next = (ctx->ev_tail + 1) % DRM_CTX_EVENT_MAX;
    if (next != ctx->ev_head) {
        struct drm_event_vblank event;
        memset(&event, 0, sizeof(event));
        uint64_t ts[2] = { 0, 0 };
        timekeeping_get_monotonic(ts);
        event.type = DRM_EVENT_FLIP_COMPLETE;
        event.length = sizeof(event);
        event.user_data = call->device->vblank.flip_user_data;
        event.tv_sec = (uint32_t)ts[0];
        event.tv_usec = (uint32_t)(ts[1] / 1000);
        event.sequence = ++call->device->vblank.sequence;
        event.crtc_id = call->device->vblank.flip_crtc_id;
        memcpy(ctx->events[ctx->ev_tail], &event, sizeof(event));
        ctx->ev_tail = next;
    }
    call->device->vblank.flip_pending = 0;
    call->device->vblank.flip_ctx = NULL;
}

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_crtc_lut {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
};

struct drm_mode_dpms {
    uint32_t dpms;
};

struct drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
};

struct drm_mode_cursor2 {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
    int32_t hot_x;
    int32_t hot_y;
};

struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
};

/* ---- helpers ----
 *
 * Everything below the "locked" suffix requires the device store mutex; the pinned variants
 * take it themselves.  No function here returns a pointer into the GEM store or
 * call->device->fbs[] unless the caller is documented to be holding the lock or a pin.
 */

static drm_gem_t *drm_find_gem_locked(drm_call_t *call, uint32_t handle)
{
    for (drm_gem_t *g = call->device->gems[handle % DRM_GEM_BUCKETS]; g; g = g->next)
        if (g->handle == handle)
            return g;
    return NULL;
}

/* Pin a GEM so its storage survives the caller dropping the lock.  Returns NULL
 * when the handle is unknown; every non-NULL return must be given back with
 * drm_gem_unpin(), which may be the call that finally frees the object. */
static drm_gem_t *drm_gem_pin(drm_call_t *call, uint32_t handle)
{
    drm_gem_t *b = NULL;
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, handle);
    if (g) {
        g->pins++;
        b = g;
    }
    drm_unlock(call);
    return b;
}

/* The storage a detached GEM still owns: releasing it can allocate, enter the
 * MM layer and talk to the host, so it happens with the store lock dropped. */
typedef struct {
    struct vmo *vmo;
    uint32_t virgl_res_id;
    drm_gem_t *object;
} drm_gem_storage_t;

static void drm_gem_drop_storage(drm_call_t *call, drm_gem_storage_t *s)
{
    if (!s->object) return;
    if (s->virgl_res_id) {
        const gpu_dev_ops_t *ops = drm_gpu_ops(call);
        int result = !ops || !ops->resource_unref ? -ENODEV :
            ops->resource_unref(drm_gpu_device(call), s->virgl_res_id);
        if (result) {
            drm_lock(call);
            s->object->next = call->device->retained;
            call->device->retained = s->object;
            drm_unlock(call);
            *s = (drm_gem_storage_t){0};
            return; /* Unknown device access: retain the original RAM owner. */
        }
        s->virgl_res_id = 0;
    }
    vmo_release(s->vmo);
    kfree(s->object);
    *s = (drm_gem_storage_t){0};
}

/* One pass per admitted operation, never a busy retry while a device fails. */
static void drm_retry_retained(drm_call_t *call)
{
    if (!call->access.publication) return;
    drm_lock(call);
    drm_gem_t *next = call->device->retained;
    call->device->retained = NULL;
    drm_unlock(call);
    while (next) {
        drm_gem_t *object = next;
        next = object->next;
        drm_gem_storage_t storage = {.vmo = object->vmo,
            .virgl_res_id = object->virgl_res_id, .object = object};
        drm_gem_drop_storage(call, &storage);
    }
}

/* Take the GEM out of the tables and hand its storage back to the caller.
 * Requires the lock and that no pin is outstanding. */
static void drm_gem_detach_locked(drm_call_t *call, drm_gem_t *b, drm_gem_storage_t *out)
{
    out->vmo = b->vmo;
    out->virgl_res_id = b->is_virgl ? b->virgl_res_id : 0;
    for (int i = 0; i < call->device->gem_name_count; i++) {
        if (call->device->gem_names[i].handle == b->handle) {
            call->device->gem_names[i] = call->device->gem_names[--call->device->gem_name_count];
            break;
        }
    }
    drm_gem_t **link = &call->device->gems[b->handle % DRM_GEM_BUCKETS];
    while (*link && *link != b)
        link = &(*link)->next;
    if (*link)
        *link = b->next;
    out->object = b;
}

/* The worker never calls an offline driver. Its publication's core receipt
 * records the accepted stop contract; a new binding cannot alter that receipt.
 * Device/store mutexes and the registry keep list/CPU owners stable here. */
static void drm_reap(void *unused)
{
    (void)unused;
    bool again = false;
    mutex_lock(&g_drm_devices_lock);
    drm_device_t **link = &g_drm_devices;
    while (*link) {
        drm_device_t *device = *link;
        if (!mutex_trylock(&device->gpu_lock)) { again = true;link = &device->next;continue; }
        drm_call_t call = {.device = device};
        bool stopped = class_device_binding_stopped(device->publication);
        if (!stopped) {
            if (!gpu_device_access_begin(device->publication, &call.access)) {
                drm_retry_retained(&call);
                drm_context_t *contexts = device->retained_contexts;
                device->retained_contexts = NULL;
                while (contexts) {
                    drm_context_t *context = contexts;
                    contexts = context->retained_next;
                    const gpu_dev_ops_t *ops = drm_gpu_ops(&call);
                    int result = !ops->ctx_destroy ? -EOPNOTSUPP :
                        ops->ctx_destroy(drm_gpu_device(&call), context->virtgpu_ctx_id);
                    if (result) {
                        context->retained_next = device->retained_contexts;
                        device->retained_contexts = context;
                    } else kfree(context);
                }
                gpu_device_access_end(&call.access);
            }
        } else {
            /* No driver/module callback after stop. Existing dma-buf pins and
             * VMA VMO references retain independent CPU storage as needed. */
            drm_lock(&call);
            memset(device->fbs, 0, sizeof(device->fbs));
            memset(&device->crtc, 0, sizeof(device->crtc));
            device->gem_name_count = 0;
            for (unsigned bucket = 0; bucket < DRM_GEM_BUCKETS; bucket++) {
                drm_gem_t *gem = device->gems[bucket];
                while (gem) {
                    drm_gem_t *next = gem->next;
                    gem->is_virgl = 0;gem->virgl_res_id = 0;
                    gem->backing_attached = 0;gem->ctx_attached_for = 0;
                    gem->fb_refs = 0;gem->dumb_live = 0;
                    if (!gem->pins) {
                        drm_gem_storage_t storage = {0};
                        drm_gem_detach_locked(&call, gem, &storage);
                        drm_unlock(&call);
                        drm_gem_drop_storage(&call, &storage);
                        drm_lock(&call);
                    }
                    gem = next;
                }
            }
            drm_gem_t *retained = device->retained;
            device->retained = NULL;
            drm_unlock(&call);
            while (retained) {
                drm_gem_t *gem = retained;retained = gem->next;
                drm_gem_storage_t storage = {.vmo = gem->vmo, .object = gem};
                drm_gem_drop_storage(&call, &storage);
            }
            while (device->retained_contexts) {
                drm_context_t *context = device->retained_contexts;
                device->retained_contexts = context->retained_next;
                kfree(context);
            }
        }
        bool empty = !device->retained && !device->retained_contexts;
        for (unsigned bucket = 0; bucket < DRM_GEM_BUCKETS; bucket++)
            if (device->gems[bucket]) empty = false;
        bool reclaim = stopped && !device->files && !device->operations && empty;
        bool pending = (!stopped && !drm_device_online(device)) ||
                       device->retained || device->retained_contexts;
        mutex_unlock(&device->gpu_lock);
        if (reclaim) {
            *link = device->next;
            class_device_put(device->publication);
            kfree(device);
        } else {
            if (pending) again = true;
            link = &device->next;
        }
    }
    mutex_unlock(&g_drm_devices_lock);
    if (again) (void)driver_work_schedule(g_drm_reaper_work, clock_get_ticks() + MS_TO_TICKS(100));
}

/* Task context, within a device operation. Detach under the store mutex,
 * then release storage or retain failed hardware cleanup outside it. */
static void drm_gem_unpin(drm_call_t *call, drm_gem_t *b)
{
    drm_gem_storage_t s = { 0 };
    if (!b)
        return;
    drm_lock(call);
    /* Saturating: a stray extra unpin would otherwise drive pins to -1, after
     * which pins == 0 is never true again and the buffer is never reclaimed --
     * a silent permanent leak of the VMO and the host resource. Turning that
     * mistake into a no-op costs one comparison. */
    if (b->pins <= 0) {
        drm_unlock(call);
        return;
    }
    b->pins--;
    if (b->pins == 0 && b->fb_refs == 0 && !b->dumb_live)
        drm_gem_detach_locked(call, b, &s);
    drm_unlock(call);
    drm_gem_drop_storage(call, &s);
}

/* Userspace dropping the dumb-buffer handle does not free storage that a live
 * framebuffer still displays; the VMO is reclaimed by drm_fb_release(). */
static void drm_free_gem(drm_call_t *call, uint32_t handle)
{
    drm_gem_storage_t s = { 0 };
    drm_lock(call);
    drm_gem_t *b = drm_find_gem_locked(call, handle);
    if (b) {
        b->dumb_live = 0;
        if (b->fb_refs == 0 && b->pins == 0)
            drm_gem_detach_locked(call, b, &s);
    }
    drm_unlock(call);
    drm_gem_drop_storage(call, &s);
}

static drm_fb_t *drm_find_fb_locked(drm_call_t *call, uint32_t fb_id)
{
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (call->device->fbs[i].used && call->device->fbs[i].fb_id == fb_id)
            return &call->device->fbs[i];
    return NULL;
}

static drm_fb_t *drm_fb_alloc_locked(drm_call_t *call, uint32_t gem_handle)
{
    for (int i = 0; i < DRM_MAX_FBS; i++) {
        if (call->device->fbs[i].used)
            continue;
        call->device->fbs[i].used = 1;
        call->device->fbs[i].fb_id = call->device->fb_next_id++;
        call->device->fbs[i].gem_handle = gem_handle;
        return &call->device->fbs[i];
    }
    return NULL;
}

static void drm_fb_release(drm_call_t *call, uint32_t fb_id)
{
    drm_gem_storage_t s = { 0 };
    drm_lock(call);
    drm_fb_t *f = drm_find_fb_locked(call, fb_id);
    if (!f) {
        drm_unlock(call);
        return;
    }
    uint32_t h = f->gem_handle;
    memset(f, 0, sizeof(*f));
    /* A framebuffer can be destroyed while it is still the one bound to the
     * CRTC. Linux lets that happen, and the CRTC binding has to follow the
     * object rather than outlive it -- otherwise GETCRTC and GETPLANE keep
     * reporting an fb_id that GETFB now answers -ENOENT for, and a client
     * cannot tell a live binding from a freed one. */
    if (call->device->crtc.fb_id == fb_id) {
        call->device->crtc.fb_id = 0;
        call->device->crtc.x = 0;
        call->device->crtc.y = 0;
    }
    drm_gem_t *b = drm_find_gem_locked(call, h);
    if (b && b->fb_refs > 0) {
        b->fb_refs--;
        if (b->fb_refs == 0 && !b->dumb_live && b->pins == 0)
            drm_gem_detach_locked(call, b, &s);
    }
    drm_unlock(call);
    drm_gem_drop_storage(call, &s);
}

/*
 * Allocate a GEM object backed by a fresh anonymous VMO.  Both the GEM and the
 * dumb-buffer paths funnel through here so there is exactly one place that
 * hands out handles and VMOs.
 */
static drm_gem_t *drm_gem_alloc_locked(drm_call_t *call, uint32_t width, uint32_t height,
                                       uint32_t pitch, uint32_t bpp,
                                       uint32_t format, uint32_t usage,
                                       uint64_t size)
{
    /* Bound each backing VMO independently of resource format and import path. */
    if (size == 0 || size > DRM_MAX_BUFFER_BYTES)
        return NULL;
    drm_gem_t *g = kmalloc(sizeof(*g));
    if (!g)
        return NULL;
    memset(g, 0, sizeof(*g));
    g->device = call->device;
    g->vmo = vmo_create(VMO_ANONYMOUS, size, 0);
    if (!g->vmo) {
        kfree(g);
        return NULL;
    }
    uint32_t h;
    do {
        h = call->device->gem_next_handle++;
    } while (h == 0 || drm_find_gem_locked(call, h));
    g->handle = h;
    g->width = width;
    g->height = height;
    g->pitch = pitch;
    g->bpp = bpp;
    g->format = format;
    g->usage = usage;
    g->size = size;
    g->dumb_live = 1;
    g->next = call->device->gems[h % DRM_GEM_BUCKETS];
    call->device->gems[h % DRM_GEM_BUCKETS] = g;
    return g;
}

static int drm_gem_name_bind_locked(drm_call_t *call, uint32_t name, uint32_t handle)
{
    for (int i = 0; i < call->device->gem_name_count; i++) {
        if (call->device->gem_names[i].name == name) {
            call->device->gem_names[i].handle = handle;
            return 0;
        }
    }
    if (call->device->gem_name_count >= DRM_MAX_GEM_NAMES)
        return -1;
    call->device->gem_names[call->device->gem_name_count].name = name;
    call->device->gem_names[call->device->gem_name_count].handle = handle;
    call->device->gem_name_count++;
    return 0;
}

static int drm_gem_name_lookup_locked(drm_call_t *call, uint32_t name, uint32_t *handle)
{
    for (int i = 0; i < call->device->gem_name_count; i++) {
        if (call->device->gem_names[i].name == name) {
            *handle = call->device->gem_names[i].handle;
            return 0;
        }
    }
    return -ENOENT;
}

static void drm_mode_fill(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
                          uint32_t vrefresh)
{
    memset(m, 0, sizeof(*m));
    m->hdisplay = (uint16_t)w;
    m->hsync_start = (uint16_t)w;
    m->hsync_end = (uint16_t)w;
    m->htotal = (uint16_t)(w + 160);
    m->vdisplay = (uint16_t)h;
    m->vsync_start = (uint16_t)h;
    m->vsync_end = (uint16_t)h;
    m->vtotal = (uint16_t)(h + 40);
    m->clock = (uint32_t)((uint64_t)w * h * vrefresh / 1000);
    m->vrefresh = vrefresh;
    m->type = 0x40; /* DRM_MODE_TYPE_DRIVER */
    strncpy(m->name, "a20", sizeof(m->name) - 1);
}

/* ---- connector EDID ---- */

#define DRM_EDID_PROP_ID 1u
#define DRM_EDID_BLOB_ID 1u

#define DRM_PLANE_TYPE_PROP_ID 2u
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeu
#define DRM_PLANE_TYPE_PRIMARY 1u

#define DRM_IN_FORMATS_PROP_ID 3u
#define DRM_IN_FORMATS_BLOB_ID 2u

/* IN_FORMATS blob: struct drm_format_modifier_blob (little-endian), telling
 * wlroots the primary plane supports ARGB8888 + XRGB8888 with
 * DRM_FORMAT_MOD_LINEAR.  Layout: 24-byte header, then the format fourcc list
 * at formats_offset, then struct drm_format_modifier entries at
 * modifiers_offset. */
static const uint8_t g_in_formats_blob[56] = {
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,  /* version=1, flags=0 */
    0x02,0x00,0x00,0x00, 0x18,0x00,0x00,0x00,  /* count_formats=2, formats_offset=24 */
    0x01,0x00,0x00,0x00, 0x20,0x00,0x00,0x00,  /* count_modifiers=1, modifiers_offset=32 */
    0x41,0x52,0x32,0x34, 0x58,0x52,0x32,0x34,  /* "AR24" (ARGB8888), "XR24" (XRGB8888) */
    0x03,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,  /* modifier.formats bitmask=0b11 */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,  /* modifier.offset=0, pad=0 */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,  /* modifier.modifier=0 (LINEAR) */
};
/* KMS object IDs must be unique across all object types (like real DRM
 * drivers, which allocate them from a single IDR): wlroots reads object
 * properties with DRM_MODE_OBJECT_ANY and resolves the object purely by ID. */
#define DRM_CONN_ID 1u
#define DRM_ENC_ID 2u
#define DRM_CRTC_ID 3u
#define DRM_PLANE_ID 4u


static int drm_edid_valid(const uint8_t *e)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    if (memcmp(e, header, sizeof(header)) != 0)
        return 0;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + e[i]);
    return sum == 0;
}

/*
 * Build a standards-compliant base EDID block for the virtual display.
 * wlroots parses make/model/physical size through libdisplay-info, which
 * rejects malformed blocks, so every field (including the checksum) must be
 * genuinely valid.  Used when the GPU device offers no EDID of its own.
 */
static void drm_edid_synthesize(uint8_t *e, uint32_t w, uint32_t h)
{
    memset(e, 0, 128);
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memcpy(e, header, sizeof(header));

    /* Vendor/product: manufacturer PNP id "AOS", product 1, serial 1. */
    e[8] = 0x05;  /* 'A'<<2 | 'O'>>3 */
    e[9] = 0xF3;  /* 'O'<<5 | 'S' */
    e[10] = 0x01; /* product code (LE) */
    e[12] = 0x01; /* serial (LE) */
    e[16] = 1;    /* week of manufacture */
    e[17] = 36;   /* year 2026 - 1990 */
    e[18] = 1;    /* EDID version 1.4 */
    e[19] = 4;

    /* Basic display parameters: digital input, physical size, gamma 2.2. */
    e[20] = 0xA5; /* digital, 8 bpc, DisplayPort-agnostic */
    uint32_t cm_w = (w * 254 + 4800) / 9600; /* ~96 dpi estimate, cm */
    uint32_t cm_h = (h * 254 + 4800) / 9600;
    if (cm_w == 0 || cm_w > 255) cm_w = 27;
    if (cm_h == 0 || cm_h > 255) cm_h = 20;
    e[21] = (uint8_t)cm_w;
    e[22] = (uint8_t)cm_h;
    e[23] = 120;  /* gamma 2.20 */
    e[24] = 0x06; /* sRGB default, preferred timing in first DTD */

    /* sRGB chromaticity coordinates. */
    static const uint8_t chroma[10] = {
        0xA6, 0x55, 0x48, 0x9B, 0x26, 0x12, 0x50, 0x54, 0x00, 0x00
    };
    memcpy(&e[25], chroma, sizeof(chroma));

    /* Established + standard timings: none beyond the DTD below. */
    for (int i = 38; i < 54; i++)
        e[i] = 0x01;

    /* Detailed timing descriptor: 1024x768@60-style mode matching the
     * fabricated KMS mode (hsync+160, vsync+40). */
    uint32_t clock_10khz = (w * (h + 40) * 60 + 5000) / 10000;
    uint32_t hblank = 160, vblank = 40;
    uint32_t hso = 24, hsw = 96, vso = 3, vsw = 4;
    uint8_t *d = &e[54];
    d[0] = (uint8_t)(clock_10khz & 0xff);
    d[1] = (uint8_t)(clock_10khz >> 8);
    d[2] = (uint8_t)w;
    d[3] = (uint8_t)hblank;
    d[4] = (uint8_t)(((w >> 8) << 4) | (hblank >> 8));
    d[5] = (uint8_t)h;
    d[6] = (uint8_t)vblank;
    d[7] = (uint8_t)(((h >> 8) << 4) | (vblank >> 8));
    d[8] = (uint8_t)hso;
    d[9] = (uint8_t)hsw;
    d[10] = (uint8_t)((vso << 4) | vsw);
    d[11] = 0;
    d[12] = (uint8_t)((cm_w * 10) & 0xff); /* image width, mm */
    d[13] = (uint8_t)((cm_h * 10) & 0xff); /* image height, mm */
    d[14] = 0;
    d[15] = 0;
    d[16] = 0;
    d[17] = 0x1E; /* digital separate sync, +hsync/+vsync */

    /* Monitor name descriptor: becomes the wayland output model. */
    static const char name[] = "A20OS Display";
    d = &e[72];
    d[3] = 0xFC;
    for (int i = 0; i < 13; i++)
        d[5 + i] = (i < (int)sizeof(name) - 1) ? (uint8_t)name[i]
                   : (i == (int)sizeof(name) - 1) ? '\n' : ' ';

    /* Two dummy descriptors keep parsers that expect four happy. */
    e[90 + 3] = 0x10;
    e[108 + 3] = 0x10;

    uint8_t sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(256 - sum);
}

static const uint8_t *drm_edid_get(drm_call_t *call)
{
    /* The block is written exactly once and never mutated afterwards, so once
     * the ready flag is set under the store lock the returned pointer stays
     * valid with the lock dropped.  The fetch itself talks to the device and
     * would take that driver's lock, so it happens before taking ours. */
    drm_lock(call);
    int ready = call->device->edid_ready;
    drm_unlock(call);
    if (ready)
        return call->device->edid;

    uint8_t built[128];
    int done = 0;
    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (ops && ops->get_edid) {
        int n = ops->get_edid(drm_gpu_device(call), built, sizeof(built));
        if (n >= 128 && drm_edid_valid(built))
            done = 1;
    }
    if (!done) {
        uint32_t w = 1024, h = 768, bpp = 32;
        if (ops && ops->get_info)
            (void)ops->get_info(drm_gpu_device(call), &w, &h, &bpp);
        drm_edid_synthesize(built, w, h);
    }

    drm_lock(call);
    if (!call->device->edid_ready) {
        memcpy(call->device->edid, built, sizeof(call->device->edid));
        __sync_synchronize();
        call->device->edid_ready = 1;
    }
    drm_unlock(call);
    return call->device->edid;
}

/* ---- ioctl handlers ---- */

static int drm_version(drm_call_t *call, void *arg)
{
    struct drm_version v;
    if (copy_from_user(&v, arg, sizeof(v)) < 0)
        return -EFAULT;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    int virtio = ops && ops->get_features;
    const char *name = virtio ? "virtio_gpu" : "a20_kms";
    const char *date = "20260810";
    const char *desc = virtio ? "A20OS virtio-gpu" : "A20OS linear scanout KMS";
    size_t name_len = strlen(name);
    size_t date_len = strlen(date);
    size_t desc_len = strlen(desc);

    if (v.name && v.name_len >= name_len &&
        copy_to_user(v.name, name, name_len) < 0)
        return -EFAULT;
    if (v.date && v.date_len >= date_len &&
        copy_to_user(v.date, date, date_len) < 0)
        return -EFAULT;
    if (v.desc && v.desc_len >= desc_len &&
        copy_to_user(v.desc, desc, desc_len) < 0)
        return -EFAULT;

    /* Deliberately 0, not the 1 a plain DRM driver reports. Mesa's virgl winsys
     * calls virgl_drm_get_version(), which answers -EINVAL for any
     * version_major != 0; the winsys then returns NULL before creating a
     * context and the loader silently falls back to software rendering.
     * Measured: with major=1 no VIRTGPU_CONTEXT_INIT is ever issued, with
     * major=0 it is and the renderer becomes virgl. */
    v.version_major = virtio ? 0 : 1;
    v.version_minor = 1;
    v.version_patchlevel = 0;
    v.name_len = name_len;
    v.date_len = date_len;
    v.desc_len = desc_len;
    return copy_to_user(arg, &v, sizeof(v)) < 0 ? -EFAULT : 0;
}
/* DRM_CLIENT_CAP_UNIVERSAL_PLANES: the single-plane KMS model is exposed
 * both through the legacy interface and as a universal PRIMARY plane, so the
 * cap is accepted (like Linux, which stores it and changes GETPLANE).  The
 * legacy plane is already the universal primary plane, so nothing changes. */
static int drm_set_client_cap(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_set_client_cap {
        uint64_t capability;
        uint64_t value;
    } cap;
    if (copy_from_user(&cap, arg, sizeof(cap)) < 0)
        return -EFAULT;
    /* DRM_CLIENT_CAP_STEREO_3D=1, DRM_CLIENT_CAP_UNIVERSAL_PLANES=2,
     * DRM_CLIENT_CAP_ATOMIC=3.  Only UNIVERSAL_PLANES is honoured: the
     * single-plane model really is a universal PRIMARY plane, so libdrm's
     * atomic path agrees with what this driver reports.
     *
     * ATOMIC is refused rather than accepted-and-then-failed. MODE_ATOMIC
     * rejects every non-TEST_ONLY commit with -EINVAL, so advertising the cap
     * told libdrm to take the atomic path and then handed it nothing but
     * errors; the desktop only avoided a fallback storm because the session
     * sets WLR_DRM_NO_ATOMIC=1. Refusing at SET_CLIENT_CAP fails early and
     * visibly, which is what lets the client pick the legacy path itself. */
    if (cap.capability == 2)
        return 0;
    return -EINVAL;
}

static int drm_get_cap(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_get_cap c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    switch (c.capability) {
    case 0x1: /* DRM_CAP_DUMB_BUFFER */
        c.value = 1;
        break;
    case 0x2: /* DRM_CAP_VBLANK_HIGH_CRTC */
        c.value = 0;
        break;
    case 0x3: /* DRM_CAP_DUMB_PREFERRED_DEPTH */
        c.value = 32;
        break;
    case 0x4: /* DRM_CAP_DUMB_PREFER_SHADOW */
        c.value = 0;
        break;
    case 0x5: /* DRM_CAP_PRIME: single-GPU virtio-gpu exports/imports its
               * dumb buffers through the PRIME handle<->fd mapping, so both
               * the IMPORT and EXPORT caps are set. */
        c.value = 3; /* DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT */
        break;
    case 0x6: /* DRM_CAP_TIMESTAMP_MONOTONIC
               *
               * Kept at 1, and it is load-bearing rather than decorative: wlroots
               * treats it as a requirement and abandons the whole DRM backend
               * without it ("DRM_CAP_TIMESTAMP_MONOTONIC unsupported" then
               * "Failed to create DRM backend"), so zeroing this takes down the
               * desktop rather than degrading it.
               *
               * It is honest for the path that exists. The vblank event carries a
               * real CLOCK_MONOTONIC stamp (timekeeping_get_monotonic in
               * drm_vblank_deliver_due_locked), and the cap's other half --
               * DRM_IOCTL_MODE_GETTIME -- is not implemented here. Reporting the
               * capability anyway is the lesser error, because a client that
               * GETTIMEs gets an error from the default arm, while a client that
               * believes it cannot have a compositor will not start at all. */
        c.value = 1;
        break;
    case 0x7: /* DRM_CAP_ASYNC_PAGE_FLIP */
        c.value = 0;
        break;
    case 0x8: /* DRM_CAP_CURSOR_WIDTH: MODE_CURSOR is a stub, so no cursor
               * plane is exposed and the width must read as 0 rather than
               * leaving clients to guess. */
        c.value = 0;
        break;
    case 0x9: /* DRM_CAP_CURSOR_HEIGHT */
        c.value = 0;
        break;
    case 0x10: /* DRM_CAP_ADDFB2_MODIFIERS: ADDFB2 takes no modifier and every
                * buffer is linear.  Advertising this would make Mesa build a
                * modifier list and then reject our frames. */
        c.value = 0;
        break;
    case 0x11: /* DRM_CAP_PAGE_FLIP_TARGET: PAGE_FLIP targets the one CRTC and
                * never reads flip_target. */
        c.value = 0;
        break;
    case 0x12: /* DRM_CAP_CRTC_IN_VBLANK_EVENT */
        c.value = 1;
        break;
    case 0x13: /* DRM_CAP_SYNCOBJ */
        c.value = 0;
        break;
    default:
        c.value = 0;
        break;
    }
    return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

static int drm_get_magic(drm_context_t *ctx, void *arg)
{
    struct drm_auth a;
    if (copy_from_user(&a, arg, sizeof(a)) < 0)
        return -EFAULT;
    if (ctx->magic == 0)
        ctx->magic = (uint32_t)(uintptr_t)ctx + 0xA20; /* arbitrary per-open magic */
    a.magic = ctx->magic;
    return copy_to_user(arg, &a, sizeof(a)) < 0 ? -EFAULT : 0;
}

static int drm_auth_magic(drm_context_t *ctx, void *arg)
{
    struct drm_auth a;
    if (copy_from_user(&a, arg, sizeof(a)) < 0)
        return -EFAULT;
    if (!ctx->is_master)
        return -EACCES;
    return 0;
}

static int drm_set_master(drm_context_t *ctx, void *arg)
{
    (void)arg;
    if (ctx->render_only)
        return -EACCES;
    ctx->is_master = 1;
    return 0;
}

static int drm_drop_master(drm_context_t *ctx, void *arg)
{
    (void)arg;
    ctx->is_master = 0;
    return 0;
}

static int drm_mode_getresources(drm_call_t *call, void *arg)
{
    struct drm_mode_card_res res;
    if (copy_from_user(&res, arg, sizeof(res)) < 0)
        return -EFAULT;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device(call);
        (void)ops->get_info(dev, &w, &h, &bpp);
    }

    uint32_t fbs[DRM_MAX_FBS];
    uint32_t crtcs[1] = { DRM_CRTC_ID };
    uint32_t conns[1] = { DRM_CONN_ID };
    uint32_t encs[1] = { DRM_ENC_ID };
    int nfbs = 0;
    drm_lock(call);
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (call->device->fbs[i].used)
            fbs[nfbs++] = call->device->fbs[i].fb_id;
    drm_unlock(call);

    res.count_fbs = (uint32_t)nfbs;
    res.count_crtcs = 1;
    res.count_connectors = 1;
    res.count_encoders = 1;
    res.min_width = 16;
    res.max_width = w;
    res.min_height = 16;
    res.max_height = h;

    if (res.fb_id_ptr && nfbs > 0 &&
        copy_to_user((void *)(uintptr_t)res.fb_id_ptr, fbs,
                     (size_t)nfbs * sizeof(uint32_t)) < 0)
        return -EFAULT;
    if (res.crtc_id_ptr && copy_to_user((void *)(uintptr_t)res.crtc_id_ptr,
                                        crtcs, sizeof(crtcs)) < 0)
        return -EFAULT;
    if (res.connector_id_ptr &&
        copy_to_user((void *)(uintptr_t)res.connector_id_ptr, conns,
                     sizeof(conns)) < 0)
        return -EFAULT;
    if (res.encoder_id_ptr && copy_to_user((void *)(uintptr_t)res.encoder_id_ptr,
                                           encs, sizeof(encs)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &res, sizeof(res)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getcrtc(drm_call_t *call, void *arg)
{
    struct drm_mode_crtc c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device(call);
        (void)ops->get_info(dev, &w, &h, &bpp);
    }
    uint32_t bound_fb, cx, cy, nconn_state;
    uint32_t conn_ids[1] = { 0 };
    drm_lock(call);
    bound_fb = call->device->crtc.fb_id;
    cx = call->device->crtc.x;
    cy = call->device->crtc.y;
    nconn_state = call->device->crtc.count_connectors;
    if (nconn_state)
        conn_ids[0] = call->device->crtc.connector_ids[0];
    drm_unlock(call);

    c.crtc_id = DRM_CRTC_ID;
    c.gamma_size = 0;
    c.fb_id = bound_fb;
    c.x = cx;
    c.y = cy;
    /* mode_valid means "the CRTC/connector pair has a programmed mode", not
     * "a framebuffer is bound".  The connector is permanently connected here
     * and its mode comes from the device, so this stays 1 even with fb_id 0;
     * reporting 0 would tell wlroots the output has no mode at all and make it
     * abandon the output during backend init. */
    c.mode_valid = 1;
    drm_mode_fill(&c.mode, w, h, 60);
    /* Linux has GETCRTC write the CRTC's current connector list back through
     * set_connectors_ptr; libdrm reads count_connectors to size the buffer
     * and the list itself to learn the routing. */
    c.count_connectors = nconn_state;
    if (c.count_connectors && c.set_connectors_ptr &&
        copy_to_user((void *)(uintptr_t)c.set_connectors_ptr, conn_ids,
                     (size_t)c.count_connectors * sizeof(uint32_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

static int drm_mode_setcrtc(drm_call_t *call, void *arg)
{
    drm_context_t *ctx = call->context;
    struct drm_mode_crtc c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;

    uint32_t conns[1];
    uint32_t nconns = 0;
    if (c.count_connectors > 0) {
        if (c.count_connectors > 1 || !c.set_connectors_ptr)
            return -EINVAL;
        if (copy_from_user(conns, (const void *)(uintptr_t)c.set_connectors_ptr,
                           sizeof(conns)) < 0)
            return -EFAULT;
        if (conns[0] != DRM_CONN_ID)
            return -EINVAL;
        nconns = 1;
    }

    if (c.fb_id == 0) {
        /* fb_id 0 is how Linux detaches the plane, so this is a real state
         * change rather than a no-op. */
        drm_lock(call);
        call->device->crtc.fb_id = 0;
        call->device->crtc.x = 0;
        call->device->crtc.y = 0;
        call->device->crtc.count_connectors = nconns;
        call->device->crtc.connector_ids[0] = DRM_CONN_ID;
        drm_unlock(call);
        drm_console_release(ctx);
        return 0;
    }

    uint32_t gem_handle;
    drm_lock(call);
    drm_fb_t *f = drm_find_fb_locked(call, c.fb_id);
    if (f)
        gem_handle = f->gem_handle;
    else
        gem_handle = 0;
    drm_unlock(call);
    if (!f)
        return -ENOENT;

    /* Present with the lock dropped -- it copies a whole framebuffer and talks
     * to the device -- so the GEM is pinned for the duration. */
    drm_gem_t *b = drm_gem_pin(call, gem_handle);
    if (!b)
        return -ENOENT;
    /* The minimal KMS implementation presents by copying into the GPU's
     * primary scanout resource before issuing TRANSFER_TO_HOST_2D.  The CRTC's
     * recorded position is the right one here: Linux treats x/y as CRTC state
     * that SETCRTC sets, so a later page flip has to keep using it. */
    int rc = drm_present_buffer_at(call, b, c.x, c.y);
    drm_gem_unpin(call, b);
    if (rc < 0)
        return -EIO;

    drm_lock(call);
    call->device->crtc.fb_id = c.fb_id;
    call->device->crtc.x = c.x;
    call->device->crtc.y = c.y;
    call->device->crtc.count_connectors = nconns;
    call->device->crtc.connector_ids[0] = DRM_CONN_ID;
    drm_unlock(call);
    return 0;
}

static int drm_mode_getconnector(drm_call_t *call, void *arg)
{
    struct drm_mode_get_connector con;
    if (copy_from_user(&con, arg, sizeof(con)) < 0)
        return -EFAULT;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device(call);
        (void)ops->get_info(dev, &w, &h, &bpp);
    }

    struct drm_mode_modeinfo mode;
    drm_mode_fill(&mode, w, h, 60);

    con.connector_id = DRM_CONN_ID;
    con.connector_type = 15; /* DRM_MODE_CONNECTOR_VIRTUAL */
    con.connector_type_id = 1;
    con.connection = 1;      /* connected */
    con.mm_width = 0;
    con.mm_height = 0;
    con.subpixel = 0;
    con.encoder_id = DRM_ENC_ID;
    con.count_modes = 1;
    con.count_props = 1;
    con.count_encoders = 1;

    (void)drm_edid_get(call);
    if (con.modes_ptr && copy_to_user((void *)(uintptr_t)con.modes_ptr,
                                      &mode, sizeof(mode)) < 0)
        return -EFAULT;
    if (con.encoders_ptr && copy_to_user((void *)(uintptr_t)con.encoders_ptr,
                                         &(uint32_t){ DRM_ENC_ID },
                                         sizeof(uint32_t)) < 0)
        return -EFAULT;
    if (con.props_ptr && copy_to_user((void *)(uintptr_t)con.props_ptr,
                                      &(uint32_t){ DRM_EDID_PROP_ID },
                                      sizeof(uint32_t)) < 0)
        return -EFAULT;
    if (con.prop_values_ptr &&
        copy_to_user((void *)(uintptr_t)con.prop_values_ptr,
                     &(uint64_t){ DRM_EDID_BLOB_ID }, sizeof(uint64_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &con, sizeof(con)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getencoder(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_encoder e;
    if (copy_from_user(&e, arg, sizeof(e)) < 0)
        return -EFAULT;
    e.encoder_id = DRM_ENC_ID;
    e.encoder_type = 4; /* DRM_MODE_ENCODER_VIRTUAL */
    e.crtc_id = DRM_CRTC_ID;
    e.possible_crtcs = 1;
    e.possible_clones = 0;
    return copy_to_user(arg, &e, sizeof(e)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getplane(drm_call_t *call, void *arg)
{
    struct drm_mode_get_plane p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    p.plane_id = DRM_PLANE_ID;
    /* The primary plane is bound exactly when the CRTC has a framebuffer, and
     * it reports that same fb.  Linux clients read this to learn whether their
     * buffer is the one on screen. */
    uint32_t bound_fb;
    drm_lock(call);
    bound_fb = call->device->crtc.fb_id;
    drm_unlock(call);
    p.crtc_id = bound_fb ? DRM_CRTC_ID : 0;
    p.fb_id = bound_fb;
    p.possible_crtcs = 1;
    p.gamma_size = 0;
    p.count_format_types = 1;
    uint32_t fmt = 0x34325258; /* DRM_FORMAT_XRGB8888 */
    if (p.format_type_ptr && copy_to_user((void *)(uintptr_t)p.format_type_ptr,
                                          &fmt, sizeof(fmt)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getplaneres(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_plane_res r;
    if (copy_from_user(&r, arg, sizeof(r)) < 0)
        return -EFAULT;
    r.count_planes = 1;
    if (r.plane_id_ptr && copy_to_user((void *)(uintptr_t)r.plane_id_ptr,
                                       &(uint32_t){ DRM_PLANE_ID },
                                       sizeof(uint32_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &r, sizeof(r)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getfb(drm_call_t *call, void *arg)
{
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_fb_t *f = drm_find_fb_locked(call, fb.fb_id);
    drm_gem_t *b = f ? drm_find_gem_locked(call, f->gem_handle) : NULL;
    if (b) {
        fb.width = b->width;
        fb.height = b->height;
        fb.pitch = b->pitch;
        fb.bpp = b->bpp;
        fb.handle = b->handle;
    }
    drm_unlock(call);
    if (!b)
        return -ENOENT;
    fb.depth = 24;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_addfb(drm_call_t *call, void *arg)
{
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *b = drm_find_gem_locked(call, fb.handle);
    if (!b) {
        drm_unlock(call);
        return -ENOENT;
    }
    drm_fb_t *f = drm_fb_alloc_locked(call, b->handle);
    if (f) {
        b->fb_refs++;
        fb.fb_id = f->fb_id;
        fb.pitch = b->pitch;
        fb.bpp = b->bpp;
    }
    drm_unlock(call);
    if (!f)
        return -ENOMEM;
    fb.depth = 24;
    if (copy_to_user(arg, &fb, sizeof(fb)) < 0) {
        /* The slot is already allocated and referenced. Handing back -EFAULT
         * without releasing it leaks one framebuffer for the life of the guest,
         * and the caller never learns the id it would have needed to clean up. */
        drm_fb_release(call, fb.fb_id);
        return -EFAULT;
    }
    return 0;
}

static int drm_mode_addfb2(drm_call_t *call, void *arg)
{
    struct drm_mode_fb_cmd2 fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    if (fb.handles[0] == 0)
        return -EINVAL;
    drm_lock(call);
    drm_gem_t *b = drm_find_gem_locked(call, fb.handles[0]);
    if (!b) {
        drm_unlock(call);
        return -ENOENT;
    }
    drm_fb_t *f = drm_fb_alloc_locked(call, b->handle);
    if (f) {
        b->fb_refs++;
        fb.fb_id = f->fb_id;
        fb.pitches[0] = b->pitch;
    }
    drm_unlock(call);
    if (!f)
        return -ENOMEM;
    if (copy_to_user(arg, &fb, sizeof(fb)) < 0) {
        drm_fb_release(call, fb.fb_id);
        return -EFAULT;
    }
    return 0;
}

static int drm_mode_rmfb(drm_call_t *call, void *arg)
{
    uint32_t fb_id = 0;
    if (copy_from_user(&fb_id, arg, sizeof(fb_id)) < 0)
        return -EFAULT;
    drm_fb_release(call, fb_id);
    return 0;
}

static int drm_mode_pageflip(drm_call_t *call, void *arg)
{
    drm_context_t *ctx = call->context;
    struct drm_mode_crtc_page_flip pf;
    if (copy_from_user(&pf, arg, sizeof(pf)) < 0)
        return -EFAULT;
    /* pf.fb_id is a framebuffer id, not a GEM handle; the two id spaces are
     * allocated independently and only coincide while both counters are at the
     * same value, so resolving it as a handle works until it silently presents
     * the wrong buffer.  Go through the framebuffer like SETCRTC does. */
    uint32_t gem_handle, px, py;
    drm_lock(call);
    drm_fb_t *f = drm_find_fb_locked(call, pf.fb_id);
    gem_handle = f ? f->gem_handle : 0;
    px = call->device->crtc.x;
    py = call->device->crtc.y;
    drm_unlock(call);
    if (!f)
        return -ENOENT;

    int wants_event = (pf.flags & 0x1) != 0; /* DRM_MODE_PAGE_FLIP_EVENT */
    if (wants_event) {
        /* Reserve the single pending-flip slot up front; real hardware
         * rejects a second flip with EBUSY until the first completes. */
        mutex_lock(&call->device->vblank.lock);
        if (call->device->vblank.flip_pending) {
            mutex_unlock(&call->device->vblank.lock);
            return -EBUSY;
        }
        call->device->vblank.flip_pending = 1;
        call->device->vblank.flip_ctx = ctx;
        call->device->vblank.flip_user_data = pf.user_data;
        call->device->vblank.flip_crtc_id = pf.crtc_id;
        mutex_unlock(&call->device->vblank.lock);
    }

    drm_gem_t *b = drm_gem_pin(call, gem_handle);
    if (!b) {
        if (wants_event) {
            mutex_lock(&call->device->vblank.lock);
            call->device->vblank.flip_pending = 0;
            call->device->vblank.flip_ctx = NULL;
            mutex_unlock(&call->device->vblank.lock);
        }
        return -ENOENT;
    }
    /* A flip carries no position of its own; it presents at wherever the CRTC
     * was placed, which is the CRTC's recorded x/y. */
    int rc = drm_present_buffer_at(call, b, px, py);
    drm_gem_unpin(call, b);
    if (rc < 0) {
        if (wants_event) {
            mutex_lock(&call->device->vblank.lock);
            call->device->vblank.flip_pending = 0;
            call->device->vblank.flip_ctx = NULL;
            mutex_unlock(&call->device->vblank.lock);
        }
        return -EIO;
    }

    /* A flip changes which buffer is on screen, so the binding moves with it.
     * Without this the primary plane would keep reporting whatever SETCRTC last
     * bound while the scanout shows a different buffer. */
    drm_lock(call);
    call->device->crtc.fb_id = pf.fb_id;
    drm_unlock(call);

    if (wants_event) {
        /* Deliver the completion synchronously, exactly as the hardware
         * completes: the virtio-gpu command has already returned, so the
         * flip IS complete the moment the ioctl is about to return.  A
         * deferred delivery would leave the event invisible to wlroots'
         * poll until some other activity wakes its event loop, stalling
         * the frame pipeline on an idle desktop. */
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        mutex_lock(&call->device->vblank.lock);
        drm_vblank_deliver_due_locked(call);
        (void)wait_queue_collect_all(&call->device->vblank.waiters, 0, PROC_WAKE_EVENT,
                                     &wake_q, NULL);
        mutex_unlock(&call->device->vblank.lock);
        (void)proc_wake_q_flush(&wake_q);
    }
    return 0;
}

static int drm_mode_dpms(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_dpms d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    (void)d.dpms;
    return 0;
}

static int drm_mode_cursor(drm_context_t *ctx, void *arg)
{
    (void)arg;
    (void)ctx;
    /* A20OS virtio-gpu has no hardware cursor plane; accept the request so
     * wlroots' legacy page-flip path can proceed. */
    return 0;
}

static int drm_mode_cursor2(drm_context_t *ctx, void *arg)
{
    (void)arg;
    (void)ctx;
    return 0;
}

static int drm_mode_getgamma(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    /* There is no gamma ramp in the model, but the caller's struct is an
     * out-parameter: returning success without writing it hands back
     * whatever was already on the user's stack.  Report a flat ramp. */
    struct drm_crtc_gamma g;
    memset(&g, 0, sizeof(g));
    g.red = 0xffff;
    g.green = 0xffff;
    g.blue = 0xffff;
    return copy_to_user(arg, &g, sizeof(g)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getproperty(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_property p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    memset(p.name, 0, sizeof(p.name));
    if (p.prop_id == DRM_EDID_PROP_ID) {
        strncpy(p.name, "EDID", sizeof(p.name) - 1);
        p.flags = 0x10 | 0x04; /* DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE */
        p.count_values = 1;
        p.count_enum_blobs = 0;
        if (p.values_ptr && copy_to_user((void *)(uintptr_t)p.values_ptr,
                                         &(uint64_t){ DRM_EDID_BLOB_ID },
                                         sizeof(uint64_t)) < 0)
            return -EFAULT;
    } else if (p.prop_id == DRM_PLANE_TYPE_PROP_ID) {
        /* wlroots reads the plane "type" property (by name) to classify the
         * plane; only the name and prop_id are needed for that match. */
        strncpy(p.name, "type", sizeof(p.name) - 1);
        p.flags = 0x08 | 0x04; /* DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE */
        p.count_values = 0;
        p.count_enum_blobs = 0;
    } else if (p.prop_id == DRM_IN_FORMATS_PROP_ID) {
        strncpy(p.name, "IN_FORMATS", sizeof(p.name) - 1);
        p.flags = 0x10 | 0x04; /* DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE */
        p.count_values = 1;
        p.count_enum_blobs = 0;
        if (p.values_ptr && copy_to_user((void *)(uintptr_t)p.values_ptr,
                                         &(uint64_t){ DRM_IN_FORMATS_BLOB_ID },
                                         sizeof(uint64_t)) < 0)
            return -EFAULT;
    } else {
        return -ENOENT;
    }
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getpropblob(drm_call_t *call, void *arg)
{
    struct drm_mode_get_blob b;
    if (copy_from_user(&b, arg, sizeof(b)) < 0)
        return -EFAULT;
    const uint8_t *data = NULL;
    uint32_t len = 0;
    if (b.blob_id == DRM_EDID_BLOB_ID) {
        data = drm_edid_get(call);
        len = 128;
    } else if (b.blob_id == DRM_IN_FORMATS_BLOB_ID) {
        data = g_in_formats_blob;
        len = sizeof(g_in_formats_blob);
    } else {
        return -ENOENT;
    }
    /* length is the caller's buffer size on the way in and the blob's real size
     * on the way out. Reporting success without copying -- which is what a short
     * or absent b.data used to do here -- hands the caller its own uninitialised
     * stack as an EDID, so a corrupt monitor is reported where an error belongs.
     * Linux answers -ENOSPC with the required length, letting the caller size
     * its buffer and retry. */
    if (!b.data)
        return -EFAULT;
    uint32_t cap = b.length;
    b.length = len;
    if (cap < len) {
        (void)copy_to_user(arg, &b, sizeof(b));
        return -ENOSPC;
    }
    if (copy_to_user((void *)(uintptr_t)b.data, data, len) < 0)
        return -EFAULT;
    return copy_to_user(arg, &b, sizeof(b)) < 0 ? -EFAULT : 0;
}

static int drm_mode_setproperty(drm_context_t *ctx, void *arg)
{
    (void)arg;
    (void)ctx;
    return 0;
}

static int drm_mode_obj_getproperties(drm_call_t *call, void *arg)
{
    struct drm_mode_obj_get_properties o;
    if (copy_from_user(&o, arg, sizeof(o)) < 0)
        return -EFAULT;
    /* Object IDs are unique across types, so resolve purely by obj_id.  This
     * handles both specific obj_type queries and DRM_MODE_OBJECT_ANY (which
     * wlroots' get_drm_prop uses to read a property value by object ID). */
    if (o.obj_id == DRM_CONN_ID) {
        /* The connector carries a single immutable EDID blob property. */
        o.count_props = 1;
        (void)drm_edid_get(call);
        if (o.props_ptr && copy_to_user((void *)(uintptr_t)o.props_ptr,
                                        &(uint32_t){ DRM_EDID_PROP_ID },
                                        sizeof(uint32_t)) < 0)
            return -EFAULT;
        if (o.prop_values_ptr &&
            copy_to_user((void *)(uintptr_t)o.prop_values_ptr,
                         &(uint64_t){ DRM_EDID_BLOB_ID },
                         sizeof(uint64_t)) < 0)
            return -EFAULT;
    } else if (o.obj_id == DRM_PLANE_ID) {
        /* The single plane exposes an immutable "type" property set to
         * DRM_PLANE_TYPE_PRIMARY so universal-plane clients (wlroots) find a
         * primary plane, plus the immutable IN_FORMATS blob advertising the
         * supported format/modifier set. */
        static const uint32_t ids[2] = { DRM_PLANE_TYPE_PROP_ID, DRM_IN_FORMATS_PROP_ID };
        static const uint64_t vals[2] = { DRM_PLANE_TYPE_PRIMARY, DRM_IN_FORMATS_BLOB_ID };
        o.count_props = 2;
        if (o.props_ptr && copy_to_user((void *)(uintptr_t)o.props_ptr,
                                        ids, sizeof(ids)) < 0)
            return -EFAULT;
        if (o.prop_values_ptr &&
            copy_to_user((void *)(uintptr_t)o.prop_values_ptr,
                         vals, sizeof(vals)) < 0)
            return -EFAULT;
    } else {
        o.count_props = 0;
    }
    return copy_to_user(arg, &o, sizeof(o)) < 0 ? -EFAULT : 0;
}

static int drm_mode_atomic(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_atomic a;
    if (copy_from_user(&a, arg, sizeof(a)) < 0)
        return -EFAULT;
    /* Accept test-only atomic commits (no-op) so atomic-capable userland
     * can probe; reject real commits until a full atomic state exists. */
    if (a.flags & 0x0100) /* DRM_MODE_ATOMIC_TEST_ONLY */
        return 0;
    return -EINVAL;
}

static int drm_gem_create(drm_call_t *call, void *arg)
{
    struct drm_gem_create c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    if (c.size == 0 || c.size > DRM_MAX_BUFFER_BYTES)
        return -EINVAL;

    drm_lock(call);
    drm_gem_t *g = drm_gem_alloc_locked(call, c.width, c.height, 0, c.bpp, c.format,
                                        0, c.size);
    if (g)
        c.handle = g->handle;
    drm_unlock(call);
    if (!g)
        return -ENOMEM;
    if (copy_to_user(arg, &c, sizeof(c)) < 0) {
        drm_free_gem(call, c.handle);
        return -EFAULT;
    }
    return 0;
}

static int drm_gem_mmap_ioctl(drm_call_t *call, void *arg)
{
    struct drm_gem_mmap m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, m.handle);
    if (g)
        m.offset = (uint64_t)g->handle * PAGE_SIZE;
    drm_unlock(call);
    if (!g)
        return -ENOENT;
    /* Same encoding as MAP_DUMB, so drm_linux_mmap(call) serves both. */
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_gem_flink(drm_call_t *call, void *arg)
{
    struct drm_gem_flink h;
    if (copy_from_user(&h, arg, sizeof(h)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, h.handle);
    if (!g) {
        drm_unlock(call);
        return -ENOENT;
    }
    h.handle = g->handle;
    /* A full table used to be swallowed, so the export returned success and the
     * name was simply never bound -- the failure then surfaced much later as a
     * GEM_OPEN -ENOENT with nothing to connect it to. */
    if (drm_gem_name_bind_locked(call, h.name, g->handle) < 0) {
        drm_unlock(call);
        return -ENOSPC;
    }
    drm_unlock(call);
    return copy_to_user(arg, &h, sizeof(h)) < 0 ? -EFAULT : 0;
}

static int drm_gem_open(drm_call_t *call, void *arg)
{
    struct drm_gem_open o;
    if (copy_from_user(&o, arg, sizeof(o)) < 0)
        return -EFAULT;
    drm_lock(call);
    uint32_t handle = 0;
    int rc = drm_gem_name_lookup_locked(call, o.name, &handle);
    drm_gem_t *g = rc == 0 ? drm_find_gem_locked(call, handle) : NULL;
    if (g)
        o.size = g->size;
    drm_unlock(call);
    if (rc < 0)
        return rc;
    if (!g)
        return -ENOENT;
    o.handle = handle;
    return copy_to_user(arg, &o, sizeof(o)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getfb2(drm_call_t *call, void *arg)
{
    struct drm_mode_fb_cmd2 fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    /* This used to zero the struct and report success, which made every lookup
     * of a real framebuffer look like a lookup of framebuffer 0 with no
     * geometry.  Answer it from the backing GEM like GETFB does. */
    drm_lock(call);
    drm_fb_t *f = drm_find_fb_locked(call, fb.fb_id);
    drm_gem_t *b = f ? drm_find_gem_locked(call, f->gem_handle) : NULL;
    memset(&fb, 0, sizeof(fb));
    if (b) {
        fb.fb_id = f->fb_id;
        fb.width = b->width;
        fb.height = b->height;
        fb.pixel_format = b->format;
        fb.pitches[0] = b->pitch;
        fb.handles[0] = b->handle;
    }
    drm_unlock(call);
    if (!b)
        return -ENOENT;
    /* One GEM object, linear layout, no modifier.  ADDFB2 does not accept a
     * modifier today, so reporting one here would be a lie; leave it 0. */
    fb.modifier[0] = 0;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_create_dumb(drm_call_t *call, void *arg)
{
    struct drm_mode_create_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    if (d.width == 0 || d.height == 0 || d.bpp == 0)
        return -EINVAL;
    if (d.flags != 0)
        return -EINVAL;

    uint32_t pitch = 0;
    uint64_t size = 0;
    if (drm_dumb_layout(d.width, d.height, d.bpp, &pitch, &size) < 0)
        return -EINVAL;

    drm_lock(call);
    drm_gem_t *b = drm_gem_alloc_locked(call, d.width, d.height, pitch, d.bpp, 0,
                                        DRM_BO_USE_LINEAR, size);
    if (b)
        d.handle = b->handle;
    drm_unlock(call);
    if (!b)
        return -ENOMEM;

    d.pitch = pitch;
    d.size = size;
    if (copy_to_user(arg, &d, sizeof(d)) < 0) {
        drm_free_gem(call, d.handle);
        return -EFAULT;
    }
    return 0;
}

static int drm_mode_map_dumb(drm_call_t *call, void *arg)
{
    struct drm_mode_map_dumb m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *b = drm_find_gem_locked(call, m.handle);
    if (b)
        m.offset = (uint64_t)b->handle * PAGE_SIZE;
    drm_unlock(call);
    if (!b)
        return -ENOENT;
    /* The Linux DRM ABI uses the fake offset handed back here as the
     * argument of a later mmap(fd, ...) call.  Return a page-aligned fake
     * offset derived from the handle; drm_linux_mmap() maps the buffer VMO
     * when the process calls mmap on /dev/dri/card0 with that offset. */
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_mode_destroy_dumb(drm_call_t *call, void *arg)
{
    struct drm_mode_destroy_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    drm_lock(call);
    int found = drm_find_gem_locked(call, d.handle) != NULL;
    drm_unlock(call);
    if (!found)
        return -ENOENT;
    drm_free_gem(call, d.handle);
    return 0;
}

static int drm_gem_close(drm_call_t *call, void *arg)
{
    struct drm_gem_close c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    drm_lock(call);
    int found = drm_find_gem_locked(call, c.handle) != NULL;
    drm_unlock(call);
    if (!found)
        return -ENOENT;
    drm_free_gem(call, c.handle);
    return 0;
}

/* Both DRM files and exported dma-bufs map the same native RAM VMO. */
static int64_t drm_map_vmo(drm_gem_t *gem, uint64_t addr, size_t len,
                            int prot, int flags, uint64_t offset)
{
    if (!len || len > SIZE_MAX - (PAGE_SIZE - 1) ||
        !prot || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
        !(flags & MAP_SHARED) || (flags & ~(MAP_SHARED | MAP_FIXED | MAP_FIXED_NOREPLACE)) ||
        (offset & (PAGE_SIZE - 1)) || gem->size > SIZE_MAX - (PAGE_SIZE - 1) ||
        addr != (vaddr_t)addr) return -EINVAL;
    if (prot & PROT_EXEC) return -EACCES;
    size_t rounded = ROUND_UP(len, PAGE_SIZE);
    size_t extent = ROUND_UP((size_t)gem->size, PAGE_SIZE);
    if (offset >= extent || rounded > extent - offset) return -EINVAL;
    task_t *task = proc_current();
    if (!task || !task->mm) return -EFAULT;
    int effective = mm_pte_flags_to_prot(mm_prot_to_pte_flags(prot));
    vaddr_t mapped = mm_mmap_vmo_cap(task->mm, (vaddr_t)addr, rounded,
        effective, flags, gem->vmo, offset, effective);
    return mm_addr_is_error(mapped) ? mm_addr_error(mapped) : (int64_t)mapped;
}

/* PRIME descriptors own the live GEM, not a snapshot of its contents. The
 * fd's pin survives SCM_RIGHTS, dup and GEM_CLOSE, and its mapping references
 * the same VMO as the DRM handle. */
static long drm_dmabuf_lseek(vfile_t *vf, long offset, int whence)
{
    drm_gem_t *g = vf->priv;
    if (offset || (whence != SEEK_SET && whence != SEEK_END))
        return -EINVAL;
    return whence == SEEK_END ? (long)g->size : 0;
}

static int64_t drm_dmabuf_mmap(vfile_t *vf, uint64_t addr, size_t len,
                               int prot, int flags, uint64_t off)
{
    drm_gem_t *gem = vf ? vf->priv : NULL;
    return gem ? drm_map_vmo(gem, addr, len, prot, flags, off) : -EBADF;
}

static int drm_dmabuf_ioctl_locked(drm_call_t *call, vfile_t *vf, unsigned long req, void *arg)
{
    if (req == 0xc0086202UL || req == 0x40086203UL) {
        struct { uint32_t flags; int32_t fd; } sync;
        if (copy_from_user(&sync, arg, sizeof(sync)) < 0)
            return -EFAULT;
        if (!sync.flags || (sync.flags & ~3U))
            return -EINVAL;
        if (req == 0x40086203UL)
            return sync_file_wait_completed(sync.fd);
        sync.fd = sync_file_create_completed("dmabuf");
        if (sync.fd < 0)
            return sync.fd;
        if (copy_to_user(arg, &sync, sizeof(sync)) < 0) {
            fdtable_close_current(sync.fd);
            return -EFAULT;
        }
        return 0;
    }
    if (req != 0x40086200UL) /* DMA_BUF_IOCTL_SYNC */
        return -ENOTTY;
    uint64_t flags;
    if (copy_from_user(&flags, arg, sizeof(flags)) < 0)
        return -EFAULT;
    if (flags & ~7ULL || !(flags & 3ULL))
        return -EINVAL;
    drm_gem_t *g = vf->priv;
    /* Synchronous GPU submission has already finished. CPU access requires
     * download on START READ and upload on END WRITE for a host resource. */
    int download = !(flags & 4) && (flags & 1);
    int upload = (flags & 4) && (flags & 2);
    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (g->is_virgl && (download || upload)) {
        if (!ops || !ops->transfer_3d)
            return -ENODEV;
        struct virtio_gpu_box box = {0, 0, 0, g->width, g->height, 1};
        if (upload) {
            for (uint64_t i = 0; i < g->size / PAGE_SIZE; i++) {
                pfn_t p = vmo_peek_page(g->vmo, i);
                if (p != PFN_NONE)
                    arch_dma_sync_for_device(pfn_to_virt(p), PAGE_SIZE);
            }
        }
        int rc = ops->transfer_3d(drm_gpu_device(call), 0, g->virgl_res_id,
                                   &box, 0, 0, 0, 0, upload);
        if (rc < 0)
            return rc;
    }
    for (uint64_t i = 0; i < g->size / PAGE_SIZE; i++) {
        pfn_t p = vmo_peek_page(g->vmo, i);
        if (p == PFN_NONE)
            continue;
        if (download)
            arch_dma_sync_for_cpu(pfn_to_virt(p), PAGE_SIZE);
        else if (upload)
            arch_dma_sync_for_device(pfn_to_virt(p), PAGE_SIZE);
    }
    return 0;
}



static int drm_dmabuf_close_active(drm_call_t *call, vfile_t *vf)
{
    drm_gem_unpin(call, vf->priv);
    vf->priv = NULL;
    return 0;
}

static int drm_dmabuf_poll_active(drm_call_t *call, vfile_t *vf, short events)
{
    (void)vf;
    if (!mutex_trylock(&call->device->gpu_lock))
        return 0;
    mutex_unlock(&call->device->gpu_lock);
    return events & (POLLIN | POLLOUT);
}

static size_t drm_dmabuf_poll_sources_active(drm_call_t *call, vfile_t *vf, short events,
                                      readiness_source_t *sources, size_t max)
{
    (void)vf; (void)events;
    if (!max)
        return 0;
    sources[0] = (readiness_source_t){ &call->device->gpu_waiters, 0, 0 };
    return 1;
}

static void drm_dmabuf_vnode_release(vnode_t *vn)
{
    kfree(vn);
}

static vnode_ops_t drm_dmabuf_vnode_ops = {
    .release = drm_dmabuf_vnode_release,
};

static int drm_dmabuf_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    drm_gem_t *gem = vf ? vf->priv : NULL;
    if (!gem) return -EBADF;
    drm_call_t call;
    int result = drm_call_begin(gem->device, NULL, &call, true);
    if (result) return result;
    drm_retry_retained(&call);
    result = drm_dmabuf_ioctl_locked(&call, vf, req, arg);
    drm_call_end(&call);
    return result;
}

static int drm_dmabuf_close(vfile_t *vf)
{
    drm_gem_t *gem = vf ? vf->priv : NULL;
    if (!gem) return 0;
    drm_call_t call;
    (void)drm_call_begin(gem->device, NULL, &call, false);
    int result = drm_dmabuf_close_active(&call, vf);
    drm_call_end(&call);
    return result;
}

static int drm_dmabuf_poll(vfile_t *vf, short events)
{
    drm_gem_t *gem = vf ? vf->priv : NULL;
    if (!gem) return POLLNVAL;
    drm_call_t call = {.device = gem->device};
    if (!drm_device_online(call.device)) return POLLHUP | POLLERR;
    return drm_dmabuf_poll_active(&call, vf, events);
}

static size_t drm_dmabuf_poll_sources(vfile_t *vf, short events,
                                       readiness_source_t *sources, size_t max)
{
    drm_gem_t *gem = vf ? vf->priv : NULL;
    if (!gem || !sources) return 0;
    drm_call_t call = {.device = gem->device};
    return drm_dmabuf_poll_sources_active(&call, vf, events, sources, max);
}

static vfile_ops_t drm_dmabuf_ops = {
    .lseek = drm_dmabuf_lseek,
    .poll = drm_dmabuf_poll,
    .poll_sources = drm_dmabuf_poll_sources,
    .ioctl = drm_dmabuf_ioctl,
    .close = drm_dmabuf_close,
    .mmap = drm_dmabuf_mmap,
};

/* Build the exporting file under the device operation. Its GEM pin owns
 * backing until fd installation finishes after the GPU mutex is released. */
static int drm_prime_export_prepare(drm_call_t *call, void *arg,
                                     struct drm_prime_handle *result, vfile_t **out)
{
    struct drm_prime_handle p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    if (p.flags & ~(O_CLOEXEC | O_RDWR))
        return -EINVAL;
    drm_gem_t *g = drm_gem_pin(call, p.handle);
    if (!g)
        return -ENOENT;
    vfile_t *vf = vfile_alloc();
    if (!vf) {
        drm_gem_unpin(call, g);
        return -ENOMEM;
    }
    vnode_t *vn = kcalloc(1, sizeof(*vn));
    if (!vn) {
        vfile_free(vf);
        drm_gem_unpin(call, g);
        return -ENOMEM;
    }
    vn->type = VFS_FT_REGULAR;
    vn->mode = S_IFREG | 0600;
    vn->size = g->size;
    vn->ino = vf->identity;
    vn->ops = &drm_dmabuf_vnode_ops;
    vnode_ref_init(vn, 1);
    vf->vnode = vn;
    vf->ops = &drm_dmabuf_ops;
    vf->priv = g;
    vf->flags = p.flags & O_RDWR ? O_RDWR : O_RDONLY;
    strcpy(vf->path, "anon_inode:[dmabuf]");
    vfile_ref_init(vf, 1);
    *result = p;
    *out = vf;
    return 0;
}

static int drm_prime_export_finish(void *arg, struct drm_prime_handle p, vfile_t *vf)
{
    vnode_t *vn = vf->vnode;
    p.fd = anonfd_install_vfile(vf, p.flags & O_CLOEXEC);
    if (p.fd < 0) {
        vnode_put(vn);
        return p.fd;
    }
    if (copy_to_user(arg, &p, sizeof(p)) < 0) {
        fdtable_close_current(p.fd);
        return -EFAULT;
    }
    return 0;
}

/* The outer adapter owns vf and the copied wire request. No VFS reference
 * operation may invoke another GPU's final close while this GPU is locked. */
static int drm_prime_fd_to_handle(drm_call_t *call, void *arg, vfile_t *vf,
                                   struct drm_prime_handle p)
{
    if (vf->ops != &drm_dmabuf_ops) return -EINVAL;
    drm_gem_t *g = vf->priv;
    if (g->device != call->device) return -EXDEV;
    drm_lock(call);
    int was_live = g->dumb_live;
    g->dumb_live = 1;
    p.handle = g->handle;
    drm_unlock(call);
    if (copy_to_user(arg, &p, sizeof(p)) < 0) {
        drm_lock(call);
        g->dumb_live = was_live;
        drm_unlock(call);
        return -EFAULT;
    }
    return 0;
}

/* ---- virtio-gpu 3D (DRM_IOCTL_VIRTGPU_*) ------------------------------- */

/* Create this open's host virgl context on first use.  Returns the context id,
 * or a negative errno.  Id 0 is not usable: the virtio-gpu protocol reserves
 * it, so start allocating at 1. */
static int drm_virtgpu_ensure_ctx(drm_call_t *call)
{
    drm_context_t *ctx = call->context;
    if (ctx->virtgpu_ctx_created > 0) return (int)ctx->virtgpu_ctx_id;
    if (ctx->virtgpu_ctx_created < 0) return -EIO;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->ctx_create || !ops->ctx_destroy)
        return -ENODEV;

    drm_lock(call);
    uint32_t id = call->device->virtgpu_next_res++;
    if (id == 0)
        id = call->device->virtgpu_next_res++;
    drm_unlock(call);
    ctx->virtgpu_ctx_id = id;
    ctx->virtgpu_ctx_created = -1; /* Unknown until the provider confirms. */
    int rc = ops->ctx_create(drm_gpu_device(call), id, 1, "a20-drm", 7);
    if (rc) return rc < 0 ? rc : -EIO;
    ctx->virtgpu_ctx_id = id;
    ctx->virtgpu_ctx_created = 1;
    return (int)id;
}

static int drm_virtgpu_getparam(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_getparam p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;

    /* p.value is a userspace *pointer*, not an in/out scalar. libdrm's
     * drmVirtgpuGetParam passes &value and reads its own local afterwards, so
     * a kernel that just stores the number into the struct leaves the client
     * with 0 for every param. virtio_gpu_drm_winsys_create() then reads
     * param_3d_features as 0 and gives up before creating a context, which is
     * why Mesa abandoned the device with every ioctl returning success. */
    uint64_t *out = (uint64_t *)(uintptr_t)p.value;
    if (!out)
        return -EFAULT;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops)
        return -ENODEV;

    switch (p.param) {
    case VIRTGPU_PARAM_3D_FEATURES:
    case VIRTGPU_PARAM_CONTEXT_INIT: {
        /* Answer from what the device negotiated, not from a constant.  A
         * client reads 3D_FEATURES before it has done anything else and
         * decides which path to take from it, so reporting 1 on a 2D-only
         * device sends every guest down the 3D path and turns "this display
         * has no virgl" into a confusing CTX_CREATE failure. */
        uint32_t f3d = 0, fctx = 0;
        if (ops->get_features)
            (void)ops->get_features(drm_gpu_device(call), &f3d, &fctx);
        p.value = (p.param == VIRTGPU_PARAM_3D_FEATURES) ? f3d : fctx;
        break;
    }
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
        /* A property of this driver's capset handling, not of the device. */
        p.value = 1;
        break;
    case VIRTGPU_PARAM_RESOURCE_BLOB:
    case VIRTGPU_PARAM_HOST_VISIBLE:
    case VIRTGPU_PARAM_CROSS_DEVICE:
    case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
        p.value = 0;
        break;
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: {
        /* Bit N set means capset id N is available.  Ask the host rather
         * than hardcoding: it is the only authority on which capsets exist. */
        uint64_t mask = 0;
        for (uint32_t idx = 0; idx < 16; idx++) {
            uint32_t id = 0, ver = 0, size = 0;
            if (!ops->capset_info || ops->capset_info(drm_gpu_device(call), idx,
                                                      &id, &ver, &size) < 0)
                break;
            if (id < 64)
                mask |= (uint64_t)1 << id;
        }
        p.value = mask;
        break;
    }
    default:
        p.value = 0;
        break;
    }
    return copy_to_user(out, &p.value, sizeof(p.value)) < 0 ? -EFAULT : 0;
}

/* Which capset GET_CAPS should ask for.
 *
 * GET_CAPS is addressed by capset *id*; only GET_CAPSET_INFO is indexed.  The
 * id is not the index: id 1 (VIRGL) is index 0, id 2 (VIRGL2) is index 1, and
 * the ids are sparse -- Venus is 4 and DRM is 6.  Sending the index where the
 * id belongs asks the host for a capset that does not exist, which comes back as
 * VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER and was previously misread as "this host
 * cannot build a GL context".
 *
 * A client that never called CONTEXT_INIT gets capset 1, which is the GL 1.x/2.x
 * protocol every virgl implementation has and the one Linux defaults to.  A
 * client that did name one is honoured only if the host advertises it, so a
 * stale id degrades to the default instead of failing the attach. */
static uint32_t drm_virtgpu_capset_id(drm_call_t *call)
{
    drm_context_t *ctx = call->context;
    uint32_t want = ctx->virtgpu_capset_id;
    if (!want)
        return VIRTIO_GPU_CAPSET_VIRGL;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->capset_info)
        return VIRTIO_GPU_CAPSET_VIRGL;
    for (uint32_t idx = 0; idx < 16; idx++) {
        uint32_t id = 0, ver = 0, size = 0;
        if (ops->capset_info(drm_gpu_device(call), idx, &id, &ver, &size) < 0)
            break;
        if (id == want)
            return want;
    }
    kinfo("[GPU] virtgpu: capset id %u not advertised by the host, using %u\n",
          want, VIRTIO_GPU_CAPSET_VIRGL);
    return VIRTIO_GPU_CAPSET_VIRGL;
}

static int drm_virtgpu_get_caps(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_get_caps c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    if (c.size == 0 || c.size > 1024 * 1024)
        return -EINVAL;

    /* Capset 6 describes this driver, not the renderer, so the kernel answers
     * it and the host is never asked. Getting this wrong is invisible from the
     * outside: the ioctl still returns success, just with the wrong capset
     * under it, and the client rejects the driver with no error of its own. */
    if (c.cap_set_id == VIRTIO_GPU_CAPSET_DRM) {
        struct virtio_gpu_drm_caps drm_caps;
        memset(&drm_caps, 0, sizeof(drm_caps));
        /* caps_set names the *renderer* capsets a client may use. It is not a
         * bitfield of request ids: capset 6 is the id this query arrives under,
         * and setting bit 6 here tells the client "no renderer available",
         * which is why an earlier revision of this made Mesa decline the driver
         * even though every virgl capset was available. Linux reports the
         * virgl capsets here, and a client reads bit 1/2 to decide. */
        drm_caps.caps_set = (1ULL << VIRTIO_GPU_CAPSET_VIRGL) |
                            (1ULL << VIRTIO_GPU_CAPSET_VIRGL2);
        drm_caps.max_version = 1;
        size_t n = c.size < sizeof(drm_caps) ? c.size : sizeof(drm_caps);
        if (copy_to_user((void *)(uintptr_t)c.addr, &drm_caps, n) < 0)
            return -EFAULT;
        c.size = (uint32_t)n;
        return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
    }

    /* The host rejects GET_CAPSET with ERR_INVALID_PARAMETER unless the
     * request names a live context, so create this open's context first. */
    int cid = drm_virtgpu_ensure_ctx(call);
    if (cid < 0)
        return cid;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->get_capset)
        return -ENODEV;

    /* kcalloc, not kmalloc: the driver fills only the bytes the host actually
     * returned, and copy_to_user must not hand the caller whatever a fresh
     * heap block happened to contain past the end of the capset. */
    uint8_t *blob = kcalloc(1, c.size);
    if (!blob)
        return -ENOMEM;
    size_t got = 0;
    uint32_t capset = c.cap_set_id ? c.cap_set_id
                                   : drm_virtgpu_capset_id(call);
    int rc = ops->get_capset(drm_gpu_device(call), (uint32_t)cid, capset,
                             c.cap_set_ver, blob, c.size, &got);
    if (rc < 0) {
        kfree(blob);
        return rc;
    }
    if (got > c.size)
        got = c.size;
    /* Report the size the host really produced, so a caller that guessed too
     * big can tell that the tail of its buffer is padding and not capset. */
    c.size = (uint32_t)got;
    if (copy_to_user((void *)(uintptr_t)c.addr, blob, got) < 0)
        rc = -EFAULT;
    kfree(blob);
    if (rc < 0)
        return rc;
    return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

/* Publish a GEM object's pages to the host as a 3D resource's backing.  Pages
 * must be materialised (charged), not merely peeked: the host writes results
 * into them, and an unmaterialised page has no frame to write to. */
static int drm_gem_attach_backing(drm_call_t *call, drm_gem_t *g)
{
    /* The pin makes the VMO and size stable, but virgl_res_id is rewritten by a
     * concurrent RESOURCE_CREATE on the same buffer. Reading it unlocked would
     * let this attach name the resource the other thread just unreffed, and the
     * backing_attached = 1 set at the end would then claim the *new* resource is
     * backed when it is not -- so the host renders into nothing, silently. */
    drm_lock(call);
    uint32_t res_id = g->is_virgl ? g->virgl_res_id : 0;
    int already = g->backing_attached;
    uint64_t gsize = g->size;
    struct vmo *gvmo = g->vmo;
    drm_unlock(call);
    if (already)
        return 0;
    if (!gvmo || gsize == 0 || !res_id)
        return -EINVAL;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->resource_attach_backing)
        return -ENODEV;

    uint32_t npages = (uint32_t)((gsize + PAGE_SIZE - 1) / PAGE_SIZE);
    if (npages == 0 || npages > 65536)
        return -EINVAL;

    struct virtio_gpu_mem_entry *entries = kmalloc(npages * sizeof(*entries));
    if (!entries)
        return -ENOMEM;

    /* The host places entry n at offset n * PAGE_SIZE, so an entry list with a
     * hole in it maps the wrong frames: if page 3 of 8 cannot be materialised
     * and the survivors are compacted, entry 3 becomes page 4's frame and the
     * renderer draws into a buffer it was never given, with no error anywhere.
     * A partial set is therefore a failure, not something to publish. */
    int rc = 0;
    for (uint32_t i = 0; i < npages; i++) {
        pfn_t pfn = PFN_NONE;
        if (vmo_get_page_charged(gvmo, i, NULL, &pfn) < 0) {
            rc = -ENOMEM;
            break;
        }
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        uint64_t len = gsize - off;
        if (len > PAGE_SIZE)
            len = PAGE_SIZE;
        entries[i].addr = (uint64_t)pfn_to_phys(pfn);
        entries[i].length = (uint32_t)len;
        entries[i].padding = 0;
    }
    if (rc == 0)
        rc = ops->resource_attach_backing(drm_gpu_device(call), res_id, entries,
                                          npages);
    if (rc > 0) rc = -EIO;
    kfree(entries);
    if (rc == 0) {
        drm_lock(call);
        /* Only claim it backed if the buffer is still the same resource. */
        if (g->is_virgl && g->virgl_res_id == res_id)
            g->backing_attached = 1;
        else
            rc = -EIO;
        drm_unlock(call);
    }
    return rc;
}

static int drm_virtgpu_resource_create(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_resource_create r;
    if (copy_from_user(&r, arg, sizeof(r)) < 0)
        return -EFAULT;
    uint64_t size = r.size ? r.size : PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (size > DRM_MAX_BUFFER_BYTES)
        return -EINVAL;

    int cid = drm_virtgpu_ensure_ctx(call);
    if (cid < 0)
        return cid;
    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->resource_create_3d)
        return -ENODEV;

    /* Linux RESOURCE_CREATE allocates a new GEM. Both handles are outputs;
     * Mesa does not preallocate one through an A20-specific ioctl. */
    drm_lock(call);
    drm_gem_t *g = drm_gem_alloc_locked(call, r.width, r.height, r.stride, 0, 0, 0, size);
    if (!g) {
        drm_unlock(call);
        return -ENOMEM;
    }
    g->pins++;
    uint32_t handle = g->handle;
    uint32_t res_id = call->device->virtgpu_next_res++;
    if (res_id == 0)
        res_id = call->device->virtgpu_next_res++;
    drm_unlock(call);

    drm_lock(call);
    g->virgl_res_id = res_id;
    g->is_virgl = 1; /* Keep the cleanup identity even if creation times out. */
    drm_unlock(call);
    int rc = ops->resource_create_3d(drm_gpu_device(call), (uint32_t)cid, res_id,
                                     r.target, r.format, r.bind, r.width,
                                     r.height, r.depth, r.array_size,
                                     r.last_level, r.nr_samples, r.flags);
    if (rc) { if (rc > 0) rc = -EIO; goto fail; }
    rc = drm_gem_attach_backing(call, g);
    if (rc < 0)
        goto fail;
    r.bo_handle = handle;
    r.res_handle = res_id;
    if (copy_to_user(arg, &r, sizeof(r)) < 0) {
        rc = -EFAULT;
        goto fail;
    }
    drm_gem_unpin(call, g);
    return 0;

fail:
    /* The pin holds the object until both GEM and host-resource ownership
     * have been dropped, including an output-copy fault. */
    drm_free_gem(call, handle);
    drm_gem_unpin(call, g);
    return rc;
}

static int drm_virtgpu_resource_info(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_resource_info i;
    if (copy_from_user(&i, arg, sizeof(i)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, i.bo_handle);
    int found = g && g->is_virgl;
    if (found) {
        i.res_handle = g->virgl_res_id;
        i.size = (uint32_t)g->size;
    }
    drm_unlock(call);
    if (!found)
        return -ENOENT;
    i.blob_mem = 0;
    return copy_to_user(arg, &i, sizeof(i)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_execbuffer(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_execbuffer e;
    if (copy_from_user(&e, arg, sizeof(e)) < 0)
        return -EFAULT;
    if (e.size == 0 || e.command == 0)
        return -EINVAL;
    if (e.flags & ~3U)
        return -EINVAL;
    if (e.flags & 1U) {
        int rc = sync_file_wait_completed(e.fence_fd);
        if (rc < 0)
            return rc;
    }
    /* Mesa's command streams routinely exceed the small staging buffer the
     * legacy private ioctl allowed; the host accepts whatever we forward. */
    if (e.size > VIRTIO_GPU_3D_MAX_CMD_BYTES)
        return -EINVAL;

    int cid = drm_virtgpu_ensure_ctx(call);
    if (cid < 0)
        return cid;

    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->submit_3d)
        return -ENODEV;

    /* Publish the buffers this frame references before the stream runs.
     * RESOURCE_CREATE already backs a resource's pages once, but a GEM that
     * became a 3D resource without that step -- and any handle Mesa adds to a
     * later frame -- would reach the host with no memory mapped, so the host
     * renders into nothing.  The command stream names bo_handles, not resource
     * ids, so the kernel is the only place that can resolve them.
     *
     * Backing is not sufficient.  virglrenderer resolves a resource through the
     * context's own list, so a resource whose pages the host has but which the
     * context has never been told about is rejected as an illegal resource --
     * and SUBMIT_3D still answers OK, which is what made this look like a
     * dropped command stream rather than a missing command. */
    if (e.num_bo_handles) {
        if (!e.bo_handles || e.num_bo_handles > 4096)
            return -EINVAL;
        uint32_t *handles = kmalloc((size_t)e.num_bo_handles * sizeof(uint32_t));
        if (!handles)
            return -ENOMEM;
        int rc = copy_from_user(handles, (const void *)(uintptr_t)e.bo_handles,
                                (size_t)e.num_bo_handles * sizeof(uint32_t));
        if (rc < 0) {
            kfree(handles);
            return -EFAULT;
        }
        for (uint32_t i = 0; i < e.num_bo_handles; i++) {
            drm_gem_t *g = drm_gem_pin(call, handles[i]);
            if (!g) {
                kfree(handles);
                return -ENOENT;
            }
            drm_lock(call);
            int usable = g->is_virgl;
            int attached = g->backing_attached;
            uint32_t res_id = g->virgl_res_id;
            uint32_t attached_for = g->ctx_attached_for;
            drm_unlock(call);
            if (!usable) {
                drm_gem_unpin(call, g);
                kfree(handles);
                return -ENOENT;
            }
            rc = attached ? 0 : drm_gem_attach_backing(call, g);
            if (rc == 0 && attached_for != (uint32_t)cid && ops->ctx_attach_resource) {
                rc = ops->ctx_attach_resource(drm_gpu_device(call), (uint32_t)cid,
                                               res_id);
                if (rc == 0) {
                    drm_lock(call);
                    /* Id-guarded: a concurrent RESOURCE_CREATE may have replaced
                     * the resource while we were off the lock, and crediting the
                     * attachment to the new id would skip the command for it. */
                    if (g->virgl_res_id == res_id)
                        g->ctx_attached_for = (uint32_t)cid;
                    drm_unlock(call);
                }
            }
            drm_gem_unpin(call, g);
            if (rc < 0) {
                kfree(handles);
                return rc;
            }
        }
        kfree(handles);
    }

    uint8_t *cmd = kmalloc(e.size);
    if (!cmd)
        return -ENOMEM;
    if (copy_from_user(cmd, (const void *)(uintptr_t)e.command, e.size) < 0) {
        kfree(cmd);
        return -EFAULT;
    }
    int rc = ops->submit_3d(drm_gpu_device(call), (uint32_t)cid, cmd, e.size);
    kfree(cmd);
    if (rc < 0)
        return rc;

    e.fence_fd = -1;
    if (e.flags & 2U) {
        e.fence_fd = sync_file_create_completed("virtgpu");
        if (e.fence_fd < 0)
            return e.fence_fd;
    }
    if (copy_to_user(arg, &e, sizeof(e)) < 0) {
        if (e.fence_fd >= 0)
            fdtable_close_current(e.fence_fd);
        return -EFAULT;
    }
    return 0;
}

static int drm_virtgpu_wait(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_3d_wait w;
    if (copy_from_user(&w, arg, sizeof(w)) < 0)
        return -EFAULT;
    /* Submission completes before the ioctl returns, so there is never
     * anything outstanding to block on.  Validating the handle still matters:
     * a blind success turns a caller that is waiting on the wrong resource
     * into a race that only shows up as corrupted pixels much later. */
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, w.handle);
    int found = g && g->is_virgl;
    drm_unlock(call);
    return found ? 0 : -ENOENT;
}

static int drm_virtgpu_map(drm_call_t *call, void *arg)
{
    struct drm_virtgpu_map m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock(call);
    drm_gem_t *g = drm_find_gem_locked(call, m.handle);
    if (g)
        m.offset = (uint64_t)g->handle * PAGE_SIZE;
    drm_unlock(call);
    if (!g)
        return -ENOENT;
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_context_init(drm_call_t *call, void *arg)
{
    drm_context_t *ctx = call->context;
    struct drm_virtgpu_context_init ci;
    if (copy_from_user(&ci, arg, sizeof(ci)) < 0)
        return -EFAULT;

    int cid = drm_virtgpu_ensure_ctx(call);
    if (cid < 0)
        return cid;

    if (ci.num_params == 0 || ci.ctx_set_params == 0)
        return 0;
    if (ci.num_params > 64)
        return -EINVAL;

    /* ctx_set_params is an array of num_params entries, not one entry: the
     * previous code read a single struct and dropped it, so a client that
     * asked for capset 2 got capset 1's protocol back with no indication. */
    struct drm_virtgpu_context_set_param *params =
        kmalloc(ci.num_params * sizeof(*params));
    if (!params)
        return -ENOMEM;
    int rc = copy_from_user(params, (const void *)(uintptr_t)ci.ctx_set_params,
                            ci.num_params * sizeof(*params));
    if (rc < 0) {
        kfree(params);
        return -EFAULT;
    }
    for (uint32_t i = 0; i < ci.num_params; i++) {
        if (params[i].param == VIRGLPARAM_CAPSET_ID)
            ctx->virtgpu_capset_id = (uint32_t)params[i].value;
        else if (params[i].param == VIRGLPARAM_CAPSET_VERSION)
            ctx->virtgpu_capset_version = (uint32_t)params[i].value;
    }
    kfree(params);
    return 0;
}

static int drm_virtgpu_transfer(drm_call_t *call, void *arg, int to_host)
{
    struct drm_virtgpu_3d_transfer t;
    if (copy_from_user(&t, arg, sizeof(t)) < 0)
        return -EFAULT;
    /* Non-blob virtgpu resources use their host format's natural layout. */
    if (t.stride || t.layer_stride || !t.box.w || !t.box.h || !t.box.d)
        return -EINVAL;
    const gpu_dev_ops_t *ops = drm_gpu_ops(call);
    if (!ops || !ops->transfer_3d)
        return -ENODEV;
    int cid = drm_virtgpu_ensure_ctx(call);
    if (cid < 0)
        return cid;
    drm_gem_t *g = drm_gem_pin(call, t.bo_handle);
    if (!g)
        return -ENOENT;
    if (!g->is_virgl || !g->vmo || t.offset >= g->size) {
        drm_gem_unpin(call, g);
        return -EINVAL;
    }
    int rc = drm_gem_attach_backing(call, g);
    if (rc == 0 && ops->ctx_attach_resource)
        rc = ops->ctx_attach_resource(drm_gpu_device(call), (uint32_t)cid,
                                       g->virgl_res_id);
    /* Formats include buffers, compressed textures, mip levels and arrays.
     * Let the host validate the box against its format; a fixed 32-bit-pixel
     * byte estimate rejects valid buffers. Sync the bounded attached VMO. */
    if (rc == 0 && to_host) {
        for (uint64_t i = 0; i < g->size / PAGE_SIZE; i++) {
            pfn_t p = vmo_peek_page(g->vmo, i);
            if (p != PFN_NONE)
                arch_dma_sync_for_device(pfn_to_virt(p), PAGE_SIZE);
        }
    }
    struct virtio_gpu_box box = {
        t.box.x, t.box.y, t.box.z, t.box.w, t.box.h, t.box.d
    };
    if (rc == 0)
        rc = ops->transfer_3d(drm_gpu_device(call), (uint32_t)cid, g->virgl_res_id,
                               &box, t.level, 0, 0, t.offset, to_host);
    if (rc == 0 && !to_host) {
        for (uint64_t i = 0; i < g->size / PAGE_SIZE; i++) {
            pfn_t p = vmo_peek_page(g->vmo, i);
            if (p != PFN_NONE)
                arch_dma_sync_for_cpu(pfn_to_virt(p), PAGE_SIZE);
        }
    }
    drm_gem_unpin(call, g);
    return rc;
}

/* ---- vfile backend ---- */

static int drm_read_active(drm_call_t *call, vfile_t *vf, char *buf, size_t count)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx || !buf)
        return -EBADF;
    if (count < 32)
        return -EINVAL;

    for (;;) {
        mutex_lock(&call->device->vblank.lock);
        drm_vblank_deliver_due_locked(call);
        if (ctx->ev_head != ctx->ev_tail) {
            /* libdrm reads with a large buffer and handles several events
             * per read; drain as many complete records as fit. */
            size_t n = 0;
            while (ctx->ev_head != ctx->ev_tail && n + 32 <= count) {
                memcpy(buf + n, ctx->events[ctx->ev_head], 32);
                ctx->ev_head = (ctx->ev_head + 1) % DRM_CTX_EVENT_MAX;
                n += 32;
            }
            mutex_unlock(&call->device->vblank.lock);
            return (int)n;
        }
        if (!drm_device_online(call->device)) {
            mutex_unlock(&call->device->vblank.lock);
            return -ENODEV;
        }
        uint64_t deadline = 0;
        mutex_unlock(&call->device->vblank.lock);

        if (vf->flags & O_NONBLOCK)
            return -EAGAIN;

        /* Block until a future page flip posts an event to this file and
         * wakes the queue. */
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&call->device->vblank.waiters, &entry, token, 0);
        mutex_lock(&call->device->vblank.lock);
        bool ready = ctx->ev_head != ctx->ev_tail || !drm_device_online(call->device);
        mutex_unlock(&call->device->vblank.lock);
        proc_wake_reason_t reason;
        if (linked && !ready)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&call->device->vblank.waiters, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
    }
}

static int drm_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    return (int)count;
}

static long drm_lseek(vfile_t *vf, long offset, int whence)
{
    (void)vf;
    (void)offset;
    (void)whence;
    return 0;
}

static int drm_poll_active(drm_call_t *call, vfile_t *vf, short events)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return POLLNVAL;
    short revents = 0;
    /* A page-flip completion becomes visible at its vblank deadline; report
     * the fd readable only while a completed event is actually queued, or
     * libdrm spins in drmHandleEvent() on an empty fd. */
    mutex_lock(&call->device->vblank.lock);
    drm_vblank_deliver_due_locked(call);
    if ((events & POLLIN) && ctx->ev_head != ctx->ev_tail)
        revents |= POLLIN;
    mutex_unlock(&call->device->vblank.lock);
    return revents;
}

static size_t drm_poll_sources_active(drm_call_t *call, vfile_t *vf, short events,
                               readiness_source_t *sources, size_t max)
{
    (void)vf; (void)events;
    if (!sources || max == 0)
        return 0;
    sources[0] = (readiness_source_t){ &call->device->vblank.waiters, 0, 0 };
    return 1;
}

static int drm_close_active(drm_call_t *call, vfile_t *vf)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (ctx) {
        /* Buffers are shared within this publication; they are reclaimed by DESTROY_DUMB / GEM_CLOSE.
         * Do not free them on close (a compositor may close the allocator fd
         * while buffers are still referenced by the backend). */
            mutex_lock(&call->device->vblank.lock);
        if (call->device->vblank.flip_ctx == ctx) {
            call->device->vblank.flip_pending = 0;
            call->device->vblank.flip_ctx = NULL;
        }
        mutex_unlock(&call->device->vblank.lock);

        drm_console_release(ctx);

        /* Releasing call->device->vblank.lock first is deliberate: the device store mutex is documented
         * as never held together with it, and this is the one path that needs
         * both.  call->device->vblank.flip_ctx can no longer name this context here, so no
         * queued event can be delivered into freed storage. */
        uint32_t cid = ctx->virtgpu_ctx_created ? ctx->virtgpu_ctx_id : 0;
        ctx->virtgpu_ctx_created = 0;
        bool retained = false;
        if (cid && !class_device_binding_stopped(call->device->publication)) {
            const gpu_dev_ops_t *ops = drm_gpu_ops(call);
            int result = !ops || !ops->ctx_destroy ? -ENODEV :
                ops->ctx_destroy(drm_gpu_device(call), cid);
            if (result) {
                ctx->virtgpu_ctx_id = cid;
                ctx->retained_next = call->device->retained_contexts;
                call->device->retained_contexts = ctx;
                retained = true;
            }
        }
        if (!retained) kfree(ctx);
        vf->priv = NULL;
    }
    return 0;
}

/* KMS is a property of the primary node. /dev/dri/renderD128 exists so a
 * rendering-only client can reach the GEM and VIRTGPU ioctls without owning the
 * display, and Linux rejects every MODE_* and master request there. This driver
 * only refused SET_MASTER, so a GBM or EGL process on the render node could
 * call MODE_SETCRTC and take the screen out from under the compositor. The
 * master flag itself was never consulted by any KMS handler, so it was never
 * protection either. */
static int drm_ioctl_needs_primary_node(unsigned long req)
{
    switch (req) {
    case DRM_IOCTL_AUTH_MAGIC:
    case DRM_IOCTL_SET_MASTER:
    case DRM_IOCTL_DROP_MASTER:
    case DRM_IOCTL_MODE_GETRESOURCES:
    case DRM_IOCTL_MODE_GETCRTC:
    case DRM_IOCTL_MODE_SETCRTC:
    case DRM_IOCTL_MODE_GETCONNECTOR:
    case DRM_IOCTL_MODE_GETENCODER:
    case DRM_IOCTL_MODE_GETPLANE:
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
    case DRM_IOCTL_MODE_GETFB:
    case DRM_IOCTL_MODE_GETFB2:
    case DRM_IOCTL_MODE_ADDFB:
    case DRM_IOCTL_MODE_ADDFB2:
    case DRM_IOCTL_MODE_RMFB:
    case DRM_IOCTL_MODE_PAGE_FLIP:
    case DRM_IOCTL_MODE_DPMS:
    case DRM_IOCTL_MODE_CURSOR:
    case DRM_IOCTL_MODE_CURSOR2:
    case DRM_IOCTL_MODE_GETGAMMA:
    case DRM_IOCTL_MODE_GETPROPERTY:
    case DRM_IOCTL_MODE_GETPROPBLOB:
    case DRM_IOCTL_MODE_SETPROPERTY:
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    case DRM_IOCTL_MODE_ATOMIC:
    case DRM_IOCTL_MODE_CREATE_DUMB:
    case DRM_IOCTL_MODE_MAP_DUMB:
    case DRM_IOCTL_MODE_DESTROY_DUMB:
        return 1;
    default:
        return 0;
    }
}

static int drm_ioctl_locked(drm_call_t *call, vfile_t *vf, unsigned long req, void *arg)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return -EBADF;
    if (ctx->render_only && drm_ioctl_needs_primary_node(req))
        return -EACCES;

    switch (req) {
    case DRM_IOCTL_VERSION:
        return drm_version(call, arg);
    case DRM_IOCTL_GET_MAGIC:
        return drm_get_magic(ctx, arg);
    case DRM_IOCTL_AUTH_MAGIC:
        return drm_auth_magic(ctx, arg);
    case DRM_IOCTL_SET_MASTER:
        return drm_set_master(ctx, arg);
    case DRM_IOCTL_DROP_MASTER:
        return drm_drop_master(ctx, arg);
    case DRM_IOCTL_GET_CAP:
        return drm_get_cap(ctx, arg);
    case DRM_IOCTL_SET_CLIENT_CAP:
        return drm_set_client_cap(ctx, arg);
    case DRM_IOCTL_GEM_CLOSE:
        return drm_gem_close(call, arg);
    case A20_GPU_IOCTL_GEM_CREATE:
        return drm_gem_create(call, arg);
    case A20_GPU_IOCTL_GEM_MMAP:
        return drm_gem_mmap_ioctl(call, arg);
    case DRM_IOCTL_GEM_FLINK:
        return drm_gem_flink(call, arg);
    case DRM_IOCTL_GEM_OPEN:
        return drm_gem_open(call, arg);
    case DRM_IOCTL_VIRTGPU_GETPARAM:
        return drm_virtgpu_getparam(call, arg);
    case DRM_IOCTL_VIRTGPU_GET_CAPS:
        return drm_virtgpu_get_caps(call, arg);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return drm_virtgpu_resource_create(call, arg);
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO:
        return drm_virtgpu_resource_info(call, arg);
    case DRM_IOCTL_VIRTGPU_EXECBUFFER:
        return drm_virtgpu_execbuffer(call, arg);
    case DRM_IOCTL_VIRTGPU_WAIT:
        return drm_virtgpu_wait(call, arg);
    case DRM_IOCTL_VIRTGPU_MAP:
        return drm_virtgpu_map(call, arg);
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT:
        return drm_virtgpu_context_init(call, arg);
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return drm_virtgpu_transfer(call, arg, 1);
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return drm_virtgpu_transfer(call, arg, 0);
    case DRM_IOCTL_MODE_GETRESOURCES:
        return drm_mode_getresources(call, arg);
    case DRM_IOCTL_MODE_GETCRTC:
        return drm_mode_getcrtc(call, arg);
    case DRM_IOCTL_MODE_SETCRTC:
        return drm_mode_setcrtc(call, arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return drm_mode_getconnector(call, arg);
    case DRM_IOCTL_MODE_GETENCODER:
        return drm_mode_getencoder(ctx, arg);
    case DRM_IOCTL_MODE_GETPLANE:
        return drm_mode_getplane(call, arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        return drm_mode_getplaneres(ctx, arg);
    case DRM_IOCTL_MODE_GETFB:
        return drm_mode_getfb(call, arg);
    case DRM_IOCTL_MODE_GETFB2:
        return drm_mode_getfb2(call, arg);
    case DRM_IOCTL_MODE_ADDFB:
        return drm_mode_addfb(call, arg);
    case DRM_IOCTL_MODE_ADDFB2:
        return drm_mode_addfb2(call, arg);
    case DRM_IOCTL_MODE_RMFB:
        return drm_mode_rmfb(call, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return drm_mode_pageflip(call, arg);
    case DRM_IOCTL_MODE_DPMS:
        return drm_mode_dpms(ctx, arg);
    case DRM_IOCTL_MODE_CURSOR:
        return drm_mode_cursor(ctx, arg);
    case DRM_IOCTL_MODE_CURSOR2:
        return drm_mode_cursor2(ctx, arg);
    case DRM_IOCTL_MODE_GETGAMMA:
        return drm_mode_getgamma(ctx, arg);
    case DRM_IOCTL_MODE_GETPROPERTY:
        return drm_mode_getproperty(ctx, arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:
        return drm_mode_getpropblob(call, arg);
    case DRM_IOCTL_MODE_SETPROPERTY:
        return drm_mode_setproperty(ctx, arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return drm_mode_obj_getproperties(call, arg);
    case DRM_IOCTL_MODE_ATOMIC:
        return drm_mode_atomic(ctx, arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return drm_mode_create_dumb(call, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
        return drm_mode_map_dumb(call, arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB:
        return drm_mode_destroy_dumb(call, arg);
    default:
        /* The A20-private 3D transport ioctls used to be forwarded to the GPU
         * driver from here.  They are gone: every one of them is covered by
         * the upstream VIRTGPU UAPI above, and keeping a second 3D ABI meant
         * two paths that can drift while only the upstream one is the one Mesa
         * ever speaks.  Unknown requests stay EINVAL.  Name the request anyway:
         * a client like Mesa walks a long ioctl path, and a silent EINVAL there
         * is indistinguishable from a client that gave up on its own. */
        kwarn("[DRM] unimplemented ioctl req=0x%lx\n", req);
        return -EINVAL;
    }
}



static int64_t drm_linux_mmap_active(drm_call_t *, vfile_t *, uint64_t,
                                     size_t, int, int, uint64_t);

static int drm_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx) return -EBADF;
    /* Resolve imports before entering any GPU operation. In particular, the
     * last put may close a dma-buf from a different device after rejection. */
    struct drm_prime_handle prime = {0};
    vfile_t *import = NULL, *export = NULL;
    if (req == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        if (!drm_device_online(ctx->device)) return -ENODEV;
        if (copy_from_user(&prime, arg, sizeof(prime)) < 0) return -EFAULT;
        import = fdtable_get_current_file_ref(prime.fd);
        if (!import) return -EBADF;
    }
    drm_call_t call;
    int result = drm_call_begin(ctx->device, ctx, &call, true);
    if (!result) {
        drm_retry_retained(&call);
        if (import)
            result = drm_prime_fd_to_handle(&call, arg, import, prime);
        else if (req == DRM_IOCTL_PRIME_HANDLE_TO_FD)
            result = drm_prime_export_prepare(&call, arg, &prime, &export);
        else
            result = drm_ioctl_locked(&call, vf, req, arg);
        drm_call_end(&call);
    }
    if (import) vfs_put_file(import);
    if (export) result = drm_prime_export_finish(arg, prime, export);
    return result;
}

static int drm_read(vfile_t *vf, char *buf, size_t bytes)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx) return -EBADF;
    drm_call_t call = {.device = ctx->device, .context = ctx};
    return drm_read_active(&call, vf, buf, bytes);
}

static int drm_poll(vfile_t *vf, short events)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx) return POLLNVAL;
    drm_call_t call = {.device = ctx->device, .context = ctx};
    int result = drm_poll_active(&call, vf, events);
    if (!drm_device_online(ctx->device)) result |= POLLHUP | POLLERR;
    return result;
}

static size_t drm_poll_sources(vfile_t *vf, short events,
                                readiness_source_t *sources, size_t max)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx || !sources) return 0;
    drm_call_t call = {.device = ctx->device, .context = ctx};
    return drm_poll_sources_active(&call, vf, events, sources, max);
}

static int drm_close(vfile_t *vf)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx) return 0;
    drm_device_t *device = ctx->device;
    drm_call_t call;
    (void)drm_call_begin(device, ctx, &call, false);
    drm_retry_retained(&call);
    int result = drm_close_active(&call, vf);
    drm_call_end(&call);
    drm_device_put(device);
    return result;
}

int64_t drm_linux_mmap(vfile_t *vf, uint64_t addr, size_t len, int prot,
                        int flags, uint64_t off)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx) return -EBADF;
    drm_call_t call;
    int result = drm_call_begin(ctx->device, ctx, &call, true);
    if (result) return result;
    int64_t mapped = drm_linux_mmap_active(&call, vf, addr, len, prot, flags, off);
    drm_call_end(&call);
    return mapped;
}

static vfile_ops_t g_drm_ops = {
    .read = drm_read,
    .write = drm_write,
    .lseek = drm_lseek,
    .ioctl = drm_ioctl,
    .poll = drm_poll,
    .poll_sources = drm_poll_sources,
    .close = drm_close,
    .mmap = drm_linux_mmap,
};

int drm_create_file(void)
{
    vfile_t *vf;
    int result = drm_open_vfile(&vf);
    return result ? result : anonfd_install_vfile(vf, 0);
}

/* Create a DRM vfile without installing it into the fd table (used by the
 * /dev/dri/card0 devfs open path, which manages fd installation itself). */
int drm_create_vfile_for(class_device_t *publication, vfile_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    struct gpu_device_access access;
    int result = gpu_device_access_begin(publication, &access);
    if (result) return result;
    if (!access.device->drv->remove) {
        gpu_device_access_end(&access);
        return -EOPNOTSUPP; /* Persistent backing requires an explicit stop contract. */
    }
    drm_context_t *ctx = kcalloc(1, sizeof(*ctx));
    vfile_t *vf = vfile_alloc();
    if (!ctx || !vf) result = -ENOMEM;
    else result = drm_device_get(publication, &ctx->device);
    if (result) {
        kfree(ctx);
        if (vf) vfile_free(vf);
    } else {
        vfile_ref_init(vf, 1);
        vf->flags = O_RDWR;
        vf->ops = &g_drm_ops;
        vf->priv = ctx;
        *out = vf;
    }
    gpu_device_access_end(&access);
    return result;
}

int drm_open_vfile(vfile_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    class_device_t *publication = gpu_device_default_get();
    int result = drm_create_vfile_for(publication, out);
    class_device_put(publication);
    return result;
}

vfile_t *drm_create_vfile(void)
{
    vfile_t *vf = NULL;
    (void)drm_open_vfile(&vf);
    return vf;
}

void drm_vfile_set_render_only(vfile_t *vf)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (ctx)
        ctx->render_only = 1;
}

void drm_device_bind(void)
{
    /* The DRM node is created lazily through devfs /dev/dri/card0; the GPU
     * driver discovery is handled by the driver core.  Nothing to do here
     * unless a hotplug event must create the node eagerly. */
}

int drm_is_drm_vfile(vfile_t *vf)
{
    return vf && vf->ops == &g_drm_ops;
}

static int64_t drm_linux_mmap_active(drm_call_t *call, vfile_t *vf,
                                     uint64_t addr, size_t len, int prot,
                                     int flags, uint64_t off)
{
    (void)vf;
    if ((off & (PAGE_SIZE - 1)) || off / PAGE_SIZE > (uint32_t)-1) return -EINVAL;
    drm_gem_t *gem = drm_gem_pin(call, (uint32_t)(off / PAGE_SIZE));
    if (!gem) return -ENOENT;
    int64_t mapped = drm_map_vmo(gem, addr, len, prot, flags, 0);
    drm_gem_unpin(call, gem);
    return mapped;
}

#ifndef CONFIG_NOMMU
/* Internal boot-test entry, never a module export or user ABI. The explicit
 * runtime gate halts on error, retaining owners for inspection. Successful
 * execution exercises real VFS, VMO/VMA and workqueue teardown on an embedded
 * GPU. It deliberately does not fabricate user pointers for ioctl usercopy. */
int drm_native_binding_selftest(class_device_t *publication)
{
#define DRM_TEST_CHECK(test) do { if (!(test)) { \
    kerr("DRIVER_DRM_BINDING: FAIL line=%d: %s\n", __LINE__, #test); \
    return -EIO; } } while (0)
    vfile_t *old_file = NULL;
    DRM_TEST_CHECK(!drm_create_vfile_for(publication, &old_file));
    drm_context_t *old_context = old_file->priv;
    drm_device_t *old_device = old_context->device;
    drm_call_t call;
    DRM_TEST_CHECK(!drm_call_begin(old_device, old_context, &call, true));
    device_t *gpu = call.access.device;
    driver_t *driver = gpu->drv;
    DRM_TEST_CHECK(driver && !driver->module && driver->remove);
    drm_lock(&call);
    drm_gem_t *old_gem = drm_gem_alloc_locked(&call, 32, 32, 128, 32, 0, 0, PAGE_SIZE);
    drm_unlock(&call);
    DRM_TEST_CHECK(old_gem);
    uint32_t old_handle = old_gem->handle;
    struct vmo *memory = old_gem->vmo;
    pfn_t page;
    DRM_TEST_CHECK(!vmo_get_page_charged(memory, 0, NULL, &page));
    mm_struct_t *mm = mm_create();
    DRM_TEST_CHECK(mm);
    vaddr_t va = mm_mmap_vmo_cap(mm, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, memory, 0, PROT_READ | PROT_WRITE);
    DRM_TEST_CHECK(!mm_addr_is_error(va));
    ((uint8_t *)pfn_to_virt(page))[0] = 0xa6;
    drm_call_end(&call);

    DRM_TEST_CHECK(!driver_unregister(driver));
    DRM_TEST_CHECK(class_device_binding_stopped(publication));
    DRM_TEST_CHECK(old_file->ops->ioctl(old_file, DRM_IOCTL_MODE_GETRESOURCES, NULL) == -ENODEV);
    DRM_TEST_CHECK((old_file->ops->poll(old_file, POLLIN) & (POLLHUP | POLLERR)) ==
                   (POLLHUP | POLLERR));
    DRM_TEST_CHECK(!driver_workqueue_flush(g_drm_reaper_queue,
                                          clock_get_ticks() + MS_TO_TICKS(2000)));
    DRM_TEST_CHECK(mm_seg_find(mm, va)->vmo == memory && refcount_read(&memory->refcount) == 1);
    DRM_TEST_CHECK(((uint8_t *)pfn_to_virt(page))[0] == 0xa6);
    DRM_TEST_CHECK(!driver_register(driver));
    class_device_t *fresh = class_device_ref_for_device(gpu);
    DRM_TEST_CHECK(fresh && fresh != publication && !class_device_binding_stopped(fresh));
    vfile_t *fresh_file = NULL;
    DRM_TEST_CHECK(!drm_create_vfile_for(fresh, &fresh_file));
    drm_context_t *fresh_context = fresh_file->priv;
    DRM_TEST_CHECK(fresh_context->device != old_device);
    DRM_TEST_CHECK(!drm_call_begin(fresh_context->device, fresh_context, &call, true));
    drm_lock(&call);
    drm_gem_t *fresh_gem = drm_gem_alloc_locked(&call, 32, 32, 128, 32, 0, 0, PAGE_SIZE);
    drm_unlock(&call);
    DRM_TEST_CHECK(fresh_gem && fresh_gem->handle == old_handle && fresh_gem->vmo != memory);
    DRM_TEST_CHECK(call.access.ops->flush && !call.access.ops->flush(gpu, 0, 0, 1, 1));
    DRM_TEST_CHECK(class_device_binding_stopped(publication));
    drm_free_gem(&call, fresh_gem->handle);
    drm_call_end(&call);
    vfs_put_file(fresh_file);
    class_device_put(fresh);
    vfs_put_file(old_file);
    DRM_TEST_CHECK(!driver_workqueue_flush(g_drm_reaper_queue,
                                          clock_get_ticks() + MS_TO_TICKS(2000)));
    bool old_retained = false;
    mutex_lock(&g_drm_devices_lock);
    for (drm_device_t *device = g_drm_devices; device; device = device->next)
        if (device->publication == publication) old_retained = true;
    mutex_unlock(&g_drm_devices_lock);
    DRM_TEST_CHECK(!old_retained && refcount_read(&memory->refcount) == 1);
    DRM_TEST_CHECK(((uint8_t *)pfn_to_virt(page))[0] == 0xa6);
    mm_destroy(mm);
    return 0;
#undef DRM_TEST_CHECK
}
#endif
