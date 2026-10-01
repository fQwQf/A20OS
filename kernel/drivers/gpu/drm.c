#include "drivers/gpu/drm.h"
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
#include "drivers/gpu/gpu_core.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/readiness.h"
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
 * The DRM device is a vfile whose private data is a per-open context that
 * holds a small dumb-buffer handle table.  KMS objects (CRTC 0, one plane,
 * one connector, one encoder) describe the single GPU scanout obtained from
 * gpu_dev_ops_t.  Dumb buffers are VMO-backed so userland can mmap them.
 */

#define DRM_MAX_GEMS 64
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
typedef struct drm_gem {
    int used;
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
    uint32_t virgl_res_id;
    int fb_refs;         /* live framebuffers referencing this GEM */
    int dumb_live;       /* userspace still holds the dumb-buffer handle */
    /* In-flight users, not owners.  Unlike fb_refs/dumb_live this never keeps
     * a buffer alive past its last owner; it only stops the storage being torn
     * down underneath a caller that must drop the store lock mid-operation. */
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

/*
 * Simulated vblank machinery.  A20OS scanout presents synchronously inside
 * the PAGE_FLIP ioctl, and the flip completion event is made visible to
 * poll/read immediately afterwards — matching how the virtio-gpu command
 * completes before the ioctl returns.  What this layer adds over the naive
 * approach is Linux-compatible *event semantics*: a per-file FIFO that
 * never drops completions (a lost page-flip event wedges wlroots' frame
 * scheduler permanently), real monotonic timestamps, a monotonically
 * increasing vblank sequence, EBUSY when a flip is still pending, and a
 * blocking read for libdrm.
 */
static struct {
    mutex_t lock;
    wait_queue_t waiters;
    int initialized;
    int flip_pending;
    drm_context_t *flip_ctx;
    uint64_t flip_user_data;
    uint32_t flip_crtc_id;
    uint32_t sequence;
} g_vblank;

static void drm_vblank_init_once(void)
{
    if (__sync_bool_compare_and_swap(&g_vblank.initialized, 0, 1)) {
        mutex_init(&g_vblank.lock);
        wait_queue_init(&g_vblank.waiters);
        __sync_synchronize();
        g_vblank.initialized = 2;
        return;
    }
    while (*(volatile int *)&g_vblank.initialized != 2)
        ;
}

/* GEM objects are global to the device, so a handle created by one open (the
 * wlroots allocator, or a GBM client on renderD128) is visible to another
 * (the wlroots backend), matching Linux DRM semantics.  wlroots exports a
 * buffer via PRIME on one fd and imports it on another, then creates an FB
 * and pages it in. */
static drm_gem_t g_gems[DRM_MAX_GEMS];
static int g_gem_count;

/* Handles are unique device-wide, not per-fd, to match the global store. */
static uint32_t g_gem_next_handle = 1;
static drm_fb_t g_fbs[DRM_MAX_FBS];
static uint32_t g_fb_next_id = 1;

/*
 * KMS state of the single CRTC.
 *
 * Linux treats a CRTC's current framebuffer, position and mode validity as
 * object state that clients read back.  Presenting without recording it makes
 * GETCRTC report fb_id 0 forever, which a client cannot distinguish from
 * "nothing has been displayed" -- and wlroots in particular reads the primary
 * plane's fb to decide whether it still owns the screen.  The primary plane
 * mirrors this binding rather than tracking a second, independently-writable
 * copy, so the two can never disagree.
 */
static struct {
    uint32_t fb_id;   /* 0 = nothing bound */
    uint32_t x;
    uint32_t y;
    uint32_t connector_ids[1];
    uint32_t count_connectors;
} g_crtc;

/* Host virgl resource ids, allocated device-wide for the same reason. */
static uint32_t g_virtgpu_next_res = 1;

/* GEM name <-> handle table backing GEM_GET_HANDLE/GEM_OPEN, so a buffer can
 * cross process boundaries by name (the export/import path GBM uses). */
static struct {
    uint32_t name;
    uint32_t handle;
} g_gem_names[DRM_MAX_GEM_NAMES];
static int g_gem_name_count;

/* PRIME fd <-> GEM handle mapping.  The dumb allocator exports a buffer
 * through one DRM open and the backend imports it through another, so the
 * mapping must be global, not per-context.
 *
 * An entry is keyed on the exported file's identity, never on the fd number
 * alone.  The kernel recycles an fd as soon as userspace closes it, so a
 * number-keyed table hands out the old GEM handle for whatever unrelated file
 * now occupies that slot -- and GEM handles are mmap-able through
 * drm_linux_mmap(), which turns a stale entry into a way to read a buffer this
 * process never owned.  vfile.identity is a monotonic per-open id, so it
 * distinguishes "the same file" from "the same number".
 *
 * No VFS call may be made while g_drm.lock is held: vfs_get_file_ref() can
 * reach a file close op, and that runs under g_file_lock.  Resolution therefore
 * happens with the store lock dropped and only the resulting numbers are
 * compared under it. */
#define DRM_PRIME_MAX 64
static struct {
    int fd;
    uint64_t identity;
    uint32_t handle;
} g_prime[DRM_PRIME_MAX];
static int g_prime_count;

/* Identity currently occupying @fd, or 0 when the slot is empty.  Caller holds
 * no store lock. */
static uint64_t drm_fd_identity(int fd)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return 0;
    uint64_t id = vf->identity;
    vfs_put_file_ref(fd, vf);
    return id;
}

/*
 * Lock for every device-global table above.
 *
 * These tables are shared by design -- a buffer created through one open has to
 * be visible to another, which is why the GEM store is device-wide rather than
 * per-fd -- but until this lock existed they were searched, mutated and torn
 * down with no mutual exclusion whatsoever.  Two ioctls running on different
 * CPUs could therefore hand out the same handle, drop an entry from a table
 * while another CPU was walking it, or free a VMO that a third ioctl was still
 * reading.  The desktop drives this from two processes at once by construction:
 * the wlroots allocator creates buffers on one fd and the backend consumes them
 * on another.
 *
 * Scope rule, because it is what decides what may be called while it is held:
 * this lock protects the tables and nothing else.  Anything that can sleep,
 * allocate, enter the VFS, or issue a virtio command is done after dropping it,
 * with the object pinned across the gap so the storage cannot vanish underneath
 * the caller.  That is why teardown is split in two -- drm_gem_detach_locked()
 * takes the object out of the tables under the lock, drm_gem_drop_storage()
 * releases the VMO and the host resource after it.
 *
 * g_drm.lock is a device-private lock and is therefore the innermost one.  It
 * must never be held together with g_vblank.lock: the close path needs both, and
 * taking them in one order here and the other in drm_close() is a deadlock.
 * drm_close() drops g_vblank.lock before acquiring this one.
 */
static struct {
    mutex_t lock;
    int initialized;
} g_drm_store;

static void drm_store_lock(void)
{
    if (__sync_bool_compare_and_swap(&g_drm_store.initialized, 0, 1)) {
        mutex_init(&g_drm_store.lock);
        __sync_synchronize();
        g_drm_store.initialized = 2;
        return;
    }
    while (*(volatile int *)&g_drm_store.initialized != 2)
        ;
}

#define drm_lock()   drm_store_lock(), mutex_lock(&g_drm_store.lock)
#define drm_unlock() mutex_unlock(&g_drm_store.lock)

static struct device *drm_gpu_device(void)
{
    return gpu_device_get_default();
}

static gpu_dev_ops_t *drm_gpu_ops(void)
{
    struct device *dev = drm_gpu_device();
    if (!dev || !dev->drv || !dev->drv->class_ops)
        return NULL;
    return (gpu_dev_ops_t *)dev->drv->class_ops;
}

/*
 * A20OS exposes a minimal KMS interface while virtio-gpu owns a separate
 * scanout allocation.  Copy a wlroots dumb buffer into that allocation before
 * asking the GPU to transfer it to the host.  Without this bridge, SETCRTC
 * and PAGE_FLIP acknowledge the commit but the host keeps displaying the
 * untouched black primary resource.
 *
 * The buffer may be smaller than the scanout and may be positioned anywhere
 * inside it; the copy is clipped to the scanout and the untouched region is
 * left alone.  Requiring an exact full-screen match, as this used to, turned
 * any smaller framebuffer into -EINVAL -- so a client that allocated for a
 * mode the display later changed could not commit at all, and the failure
 * surfaced as a compositor error rather than as a clipped blit.
 */
static int drm_present_buffer_at(drm_gem_t *b, uint32_t x, uint32_t y)
{
    static unsigned int present_count;
    if (!b || !b->vmo)
        return -EINVAL;

    struct device *dev = drm_gpu_device();
    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!dev || !ops || !ops->get_info || !ops->get_fb || !ops->flush)
        return -ENODEV;

    uint32_t width = 0, height = 0, bpp = 0;
    uintptr_t fb_phys = 0;
    size_t fb_size = 0;
    if (ops->get_info(dev, &width, &height, &bpp) < 0 ||
        ops->get_fb(dev, &fb_phys, &fb_size) < 0 ||
        bpp != 32 || fb_size < (size_t)height * width * 4)
        return -EINVAL;
    if (b->width == 0 || b->height == 0 || b->pitch < b->width * 4)
        return -EINVAL;
    if (x >= width || y >= height)
        return -EINVAL;   /* entirely off-screen */

    uint32_t rows = height - y;
    if (rows > b->height)
        rows = b->height;
    uint32_t cols = width - x;
    if (cols > b->width)
        cols = b->width;

    uint8_t *dst = (uint8_t *)pfn_to_virt(phys_to_pfn(fb_phys));
    if (!dst)
        return -EFAULT;

    for (uint32_t row = 0; row < rows; row++) {
        size_t src_offset = (size_t)row * b->pitch;
        size_t dst_offset = ((size_t)(y + row) * width + x) * 4;
        size_t remaining = (size_t)cols * 4;
        while (remaining > 0) {
            uint32_t page_index = (uint32_t)(src_offset / PAGE_SIZE);
            size_t page_offset = src_offset & (PAGE_SIZE - 1);
            size_t count = PAGE_SIZE - page_offset;
            if (count > remaining)
                count = remaining;

            pfn_t pfn = vmo_peek_page(b->vmo, page_index);
            if (pfn == PFN_NONE)
                memset(dst + dst_offset, 0, count);
            else
                memcpy(dst + dst_offset,
                       (uint8_t *)pfn_to_virt(pfn) + page_offset, count);
            src_offset += count;
            dst_offset += count;
            remaining -= count;
        }
    }

    /* Flush only the rectangle that was written.  The rest of the host's copy
     * still holds whatever the previous present left there, which is what a
     * partial present means. */
    int ret = ops->flush(dev, x, y, cols, rows);
    if (present_count < 4) {
        kinfo("[DRM] present handle=%u %ux%u at %u,%u pages=%lu flush=%d\n",
              b->handle, cols, rows, x, y,
              (unsigned long)((b->size + PAGE_SIZE - 1) / PAGE_SIZE), ret);
        present_count++;
    }
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
 * Caller holds g_vblank.lock. */
static void drm_vblank_deliver_due_locked(void)
{
    if (!g_vblank.flip_pending)
        return;

    drm_context_t *ctx = g_vblank.flip_ctx;
    unsigned next = (ctx->ev_tail + 1) % DRM_CTX_EVENT_MAX;
    if (next != ctx->ev_head) {
        struct drm_event_vblank event;
        memset(&event, 0, sizeof(event));
        uint64_t ts[2] = { 0, 0 };
        timekeeping_get_monotonic(ts);
        event.type = DRM_EVENT_FLIP_COMPLETE;
        event.length = sizeof(event);
        event.user_data = g_vblank.flip_user_data;
        event.tv_sec = (uint32_t)ts[0];
        event.tv_usec = (uint32_t)(ts[1] / 1000);
        event.sequence = ++g_vblank.sequence;
        event.crtc_id = g_vblank.flip_crtc_id;
        memcpy(ctx->events[ctx->ev_tail], &event, sizeof(event));
        ctx->ev_tail = next;
    }
    g_vblank.flip_pending = 0;
    g_vblank.flip_ctx = NULL;
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
 * Everything below the "locked" suffix requires g_drm.lock; the pinned variants
 * take it themselves.  No function here returns a pointer into g_gems[]/
 * g_fbs[] unless the caller is documented to be holding the lock or a pin.
 */

static drm_gem_t *drm_find_gem_locked(uint32_t handle)
{
    for (int i = 0; i < g_gem_count; i++)
        if (g_gems[i].used && g_gems[i].handle == handle)
            return &g_gems[i];
    return NULL;
}

/* Pin a GEM so its storage survives the caller dropping the lock.  Returns NULL
 * when the handle is unknown; every non-NULL return must be given back with
 * drm_gem_unpin(), which may be the call that finally frees the object. */
static drm_gem_t *drm_gem_pin(uint32_t handle)
{
    drm_gem_t *b = NULL;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(handle);
    if (g) {
        g->pins++;
        b = g;
    }
    drm_unlock();
    return b;
}

/* The storage a detached GEM still owns: releasing it can allocate, enter the
 * MM layer and talk to the host, so it happens with the store lock dropped. */
typedef struct {
    struct vmo *vmo;
    uint32_t virgl_res_id;
} drm_gem_storage_t;

static void drm_gem_drop_storage(drm_gem_storage_t *s)
{
    if (!s->vmo && !s->virgl_res_id)
        return;
    /* A 3D resource is still mapped by the host after userspace drops its
     * handle: the host writes rendered results straight into these frames.
     * Releasing the VMO first would hand those frames back to the allocator
     * while the host can still write to them, so drop the resource first and
     * only then free the pages. */
    if (s->virgl_res_id) {
        gpu_dev_ops_t *ops = drm_gpu_ops();
        if (ops && ops->resource_unref)
            ops->resource_unref(drm_gpu_device(), s->virgl_res_id);
        s->virgl_res_id = 0;
    }
    if (s->vmo) {
        vmo_release(s->vmo);
        s->vmo = NULL;
    }
}

/* Take the GEM out of the tables and hand its storage back to the caller.
 * Requires the lock and that no pin is outstanding. */
static void drm_gem_detach_locked(drm_gem_t *b, drm_gem_storage_t *out)
{
    out->vmo = b->vmo;
    out->virgl_res_id = b->is_virgl ? b->virgl_res_id : 0;
    for (int i = 0; i < g_gem_name_count; i++) {
        if (g_gem_names[i].handle == b->handle) {
            g_gem_names[i] = g_gem_names[--g_gem_name_count];
            break;
        }
    }
    memset(b, 0, sizeof(*b));
    b->used = 0;
}

/* Drop a pin and, if the object has no owner left either, reclaim it.  Safe to
 * call from any context: it takes the lock only around the table work and
 * releases the storage after dropping it. */
static void drm_gem_unpin(drm_gem_t *b)
{
    drm_gem_storage_t s = { 0 };
    if (!b)
        return;
    drm_lock();
    b->pins--;
    if (b->pins == 0 && b->fb_refs == 0 && !b->dumb_live)
        drm_gem_detach_locked(b, &s);
    drm_unlock();
    drm_gem_drop_storage(&s);
}

/* Userspace dropping the dumb-buffer handle does not free storage that a live
 * framebuffer still displays; the VMO is reclaimed by drm_fb_release(). */
static void drm_free_gem(uint32_t handle)
{
    drm_gem_storage_t s = { 0 };
    drm_lock();
    drm_gem_t *b = drm_find_gem_locked(handle);
    if (b) {
        b->dumb_live = 0;
        if (b->fb_refs == 0 && b->pins == 0)
            drm_gem_detach_locked(b, &s);
    }
    drm_unlock();
    drm_gem_drop_storage(&s);
}

static drm_fb_t *drm_find_fb_locked(uint32_t fb_id)
{
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (g_fbs[i].used && g_fbs[i].fb_id == fb_id)
            return &g_fbs[i];
    return NULL;
}

static drm_fb_t *drm_fb_alloc_locked(uint32_t gem_handle)
{
    for (int i = 0; i < DRM_MAX_FBS; i++) {
        if (g_fbs[i].used)
            continue;
        g_fbs[i].used = 1;
        g_fbs[i].fb_id = g_fb_next_id++;
        g_fbs[i].gem_handle = gem_handle;
        return &g_fbs[i];
    }
    return NULL;
}

static void drm_fb_release(uint32_t fb_id)
{
    drm_gem_storage_t s = { 0 };
    drm_lock();
    drm_fb_t *f = drm_find_fb_locked(fb_id);
    if (!f) {
        drm_unlock();
        return;
    }
    uint32_t h = f->gem_handle;
    memset(f, 0, sizeof(*f));
    drm_gem_t *b = drm_find_gem_locked(h);
    if (b && b->fb_refs > 0) {
        b->fb_refs--;
        if (b->fb_refs == 0 && !b->dumb_live && b->pins == 0)
            drm_gem_detach_locked(b, &s);
    }
    drm_unlock();
    drm_gem_drop_storage(&s);
}

/*
 * Allocate a GEM object backed by a fresh anonymous VMO.  Both the GEM and the
 * dumb-buffer paths funnel through here so there is exactly one place that
 * hands out handles and VMOs.
 */
static drm_gem_t *drm_gem_alloc_locked(uint32_t width, uint32_t height,
                                       uint32_t pitch, uint32_t bpp,
                                       uint32_t format, uint32_t usage,
                                       uint64_t size)
{
    if (size == 0)
        return NULL;
    for (int i = 0; i < DRM_MAX_GEMS; i++) {
        if (g_gems[i].used)
            continue;
        struct vmo *vmo = vmo_create(VMO_ANONYMOUS, size, 0);
        if (!vmo)
            return NULL;
        drm_gem_t *g = &g_gems[i];
        memset(g, 0, sizeof(*g));
        g->used = 1;
        uint32_t h;
        do {
            h = g_gem_next_handle++;
        } while (h == 0 || drm_find_gem_locked(h));
        g->handle = h;
        g->width = width;
        g->height = height;
        g->pitch = pitch;
        g->bpp = bpp;
        g->format = format;
        g->usage = usage;
        g->vmo = vmo;
        g->size = size;
        g->dumb_live = 1;
        if (i + 1 > g_gem_count)
            g_gem_count = i + 1;
        return g;
    }
    return NULL;
}

static void drm_gem_name_bind_locked(uint32_t name, uint32_t handle)
{
    for (int i = 0; i < g_gem_name_count; i++) {
        if (g_gem_names[i].name == name) {
            g_gem_names[i].handle = handle;
            return;
        }
    }
    if (g_gem_name_count >= DRM_MAX_GEM_NAMES)
        return;
    g_gem_names[g_gem_name_count].name = name;
    g_gem_names[g_gem_name_count].handle = handle;
    g_gem_name_count++;
}

static int drm_gem_name_lookup(uint32_t name, uint32_t *handle)
{
    for (int i = 0; i < g_gem_name_count; i++) {
        if (g_gem_names[i].name == name) {
            *handle = g_gem_names[i].handle;
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

static uint8_t g_edid[128];
static int g_edid_ready;

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

static const uint8_t *drm_edid_get(void)
{
    /* The block is written exactly once and never mutated afterwards, so once
     * the ready flag is set under the store lock the returned pointer stays
     * valid with the lock dropped.  The fetch itself talks to the device and
     * would take that driver's lock, so it happens before taking ours. */
    drm_lock();
    int ready = g_edid_ready;
    drm_unlock();
    if (ready)
        return g_edid;

    uint8_t built[128];
    int done = 0;
    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (ops && ops->get_edid) {
        int n = ops->get_edid(drm_gpu_device(), built, sizeof(built));
        if (n >= 128 && drm_edid_valid(built))
            done = 1;
    }
    if (!done) {
        uint32_t w = 1024, h = 768, bpp = 32;
        if (ops && ops->get_info)
            (void)ops->get_info(drm_gpu_device(), &w, &h, &bpp);
        drm_edid_synthesize(built, w, h);
    }

    drm_lock();
    if (!g_edid_ready) {
        memcpy(g_edid, built, sizeof(g_edid));
        __sync_synchronize();
        g_edid_ready = 1;
    }
    drm_unlock();
    return g_edid;
}

/* ---- ioctl handlers ---- */

static int drm_version(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_version v;
    if (copy_from_user(&v, arg, sizeof(v)) < 0)
        return -EFAULT;

    const char *name = "a20drm";
    const char *date = "20260810";
    const char *desc = "A20OS minimal DRM";
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

    v.version_major = 1;
    v.version_minor = 0;
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
     * DRM_CLIENT_CAP_ATOMIC=3.  The single-plane KMS model is exposed both
     * through the legacy interface and as a universal PRIMARY plane, so the
     * caps are accepted (like Linux, which stores them and changes the
     * object-query behaviour). */
    if (cap.capability == 2 || cap.capability == 3)
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
    case 0x6: /* DRM_CAP_TIMESTAMP_MONOTONIC */
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

static int drm_mode_getresources(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_card_res res;
    if (copy_from_user(&res, arg, sizeof(res)) < 0)
        return -EFAULT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device();
        (void)ops->get_info(dev, &w, &h, &bpp);
    }

    uint32_t fbs[DRM_MAX_FBS];
    uint32_t crtcs[1] = { DRM_CRTC_ID };
    uint32_t conns[1] = { DRM_CONN_ID };
    uint32_t encs[1] = { DRM_ENC_ID };
    int nfbs = 0;
    drm_lock();
    for (int i = 0; i < DRM_MAX_FBS; i++)
        if (g_fbs[i].used)
            fbs[nfbs++] = g_fbs[i].fb_id;
    drm_unlock();

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

static int drm_mode_getcrtc(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_crtc c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device();
        (void)ops->get_info(dev, &w, &h, &bpp);
    }
    uint32_t bound_fb, cx, cy, nconn_state;
    uint32_t conn_ids[1] = { 0 };
    drm_lock();
    bound_fb = g_crtc.fb_id;
    cx = g_crtc.x;
    cy = g_crtc.y;
    nconn_state = g_crtc.count_connectors;
    if (nconn_state)
        conn_ids[0] = g_crtc.connector_ids[0];
    drm_unlock();

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

static int drm_mode_setcrtc(drm_context_t *ctx, void *arg)
{
    (void)ctx;
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
        drm_lock();
        g_crtc.fb_id = 0;
        g_crtc.x = 0;
        g_crtc.y = 0;
        g_crtc.count_connectors = nconns;
        g_crtc.connector_ids[0] = DRM_CONN_ID;
        drm_unlock();
        return 0;
    }

    uint32_t gem_handle;
    drm_lock();
    drm_fb_t *f = drm_find_fb_locked(c.fb_id);
    if (f)
        gem_handle = f->gem_handle;
    else
        gem_handle = 0;
    drm_unlock();
    if (!f)
        return -ENOENT;

    /* Present with the lock dropped -- it copies a whole framebuffer and talks
     * to the device -- so the GEM is pinned for the duration. */
    drm_gem_t *b = drm_gem_pin(gem_handle);
    if (!b)
        return -ENOENT;
    /* The minimal KMS implementation presents by copying into the GPU's
     * primary scanout resource before issuing TRANSFER_TO_HOST_2D.  The CRTC's
     * recorded position is the right one here: Linux treats x/y as CRTC state
     * that SETCRTC sets, so a later page flip has to keep using it. */
    int rc = drm_present_buffer_at(b, c.x, c.y);
    drm_gem_unpin(b);
    if (rc < 0)
        return -EIO;

    drm_lock();
    g_crtc.fb_id = c.fb_id;
    g_crtc.x = c.x;
    g_crtc.y = c.y;
    g_crtc.count_connectors = nconns;
    g_crtc.connector_ids[0] = DRM_CONN_ID;
    drm_unlock();
    return 0;
}

static int drm_mode_getconnector(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_connector con;
    if (copy_from_user(&con, arg, sizeof(con)) < 0)
        return -EFAULT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device();
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

    (void)drm_edid_get();
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

static int drm_mode_getplane(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_plane p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    p.plane_id = DRM_PLANE_ID;
    /* The primary plane is bound exactly when the CRTC has a framebuffer, and
     * it reports that same fb.  Linux clients read this to learn whether their
     * buffer is the one on screen. */
    uint32_t bound_fb;
    drm_lock();
    bound_fb = g_crtc.fb_id;
    drm_unlock();
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

static int drm_mode_getfb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_lock();
    drm_fb_t *f = drm_find_fb_locked(fb.fb_id);
    drm_gem_t *b = f ? drm_find_gem_locked(f->gem_handle) : NULL;
    if (b) {
        fb.width = b->width;
        fb.height = b->height;
        fb.pitch = b->pitch;
        fb.bpp = b->bpp;
        fb.handle = b->handle;
    }
    drm_unlock();
    if (!b)
        return -ENOENT;
    fb.depth = 24;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_addfb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *b = drm_find_gem_locked(fb.handle);
    if (!b) {
        drm_unlock();
        return -ENOENT;
    }
    drm_fb_t *f = drm_fb_alloc_locked(b->handle);
    if (f) {
        b->fb_refs++;
        fb.fb_id = f->fb_id;
        fb.pitch = b->pitch;
        fb.bpp = b->bpp;
    }
    drm_unlock();
    if (!f)
        return -ENOMEM;
    fb.depth = 24;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_addfb2(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_fb_cmd2 fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    if (fb.handles[0] == 0)
        return -EINVAL;
    drm_lock();
    drm_gem_t *b = drm_find_gem_locked(fb.handles[0]);
    if (!b) {
        drm_unlock();
        return -ENOENT;
    }
    drm_fb_t *f = drm_fb_alloc_locked(b->handle);
    if (f) {
        b->fb_refs++;
        fb.fb_id = f->fb_id;
        fb.pitches[0] = b->pitch;
    }
    drm_unlock();
    if (!f)
        return -ENOMEM;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_rmfb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    uint32_t fb_id = 0;
    if (copy_from_user(&fb_id, arg, sizeof(fb_id)) < 0)
        return -EFAULT;
    drm_fb_release(fb_id);
    return 0;
}

static int drm_mode_pageflip(drm_context_t *ctx, void *arg)
{
    struct drm_mode_crtc_page_flip pf;
    if (copy_from_user(&pf, arg, sizeof(pf)) < 0)
        return -EFAULT;
    /* pf.fb_id is a framebuffer id, not a GEM handle; the two id spaces are
     * allocated independently and only coincide while both counters are at the
     * same value, so resolving it as a handle works until it silently presents
     * the wrong buffer.  Go through the framebuffer like SETCRTC does. */
    uint32_t gem_handle, px, py;
    drm_lock();
    drm_fb_t *f = drm_find_fb_locked(pf.fb_id);
    gem_handle = f ? f->gem_handle : 0;
    px = g_crtc.x;
    py = g_crtc.y;
    drm_unlock();
    if (!f)
        return -ENOENT;

    drm_vblank_init_once();
    int wants_event = (pf.flags & 0x1) != 0; /* DRM_MODE_PAGE_FLIP_EVENT */
    if (wants_event) {
        /* Reserve the single pending-flip slot up front; real hardware
         * rejects a second flip with EBUSY until the first completes. */
        mutex_lock(&g_vblank.lock);
        if (g_vblank.flip_pending) {
            mutex_unlock(&g_vblank.lock);
            return -EBUSY;
        }
        g_vblank.flip_pending = 1;
        g_vblank.flip_ctx = ctx;
        g_vblank.flip_user_data = pf.user_data;
        g_vblank.flip_crtc_id = pf.crtc_id;
        mutex_unlock(&g_vblank.lock);
    }

    drm_gem_t *b = drm_gem_pin(gem_handle);
    if (!b) {
        if (wants_event) {
            mutex_lock(&g_vblank.lock);
            g_vblank.flip_pending = 0;
            g_vblank.flip_ctx = NULL;
            mutex_unlock(&g_vblank.lock);
        }
        return -ENOENT;
    }
    /* A flip carries no position of its own; it presents at wherever the CRTC
     * was placed, which is the CRTC's recorded x/y. */
    int rc = drm_present_buffer_at(b, px, py);
    drm_gem_unpin(b);
    if (rc < 0) {
        if (wants_event) {
            mutex_lock(&g_vblank.lock);
            g_vblank.flip_pending = 0;
            g_vblank.flip_ctx = NULL;
            mutex_unlock(&g_vblank.lock);
        }
        return -EIO;
    }

    /* A flip changes which buffer is on screen, so the binding moves with it.
     * Without this the primary plane would keep reporting whatever SETCRTC last
     * bound while the scanout shows a different buffer. */
    drm_lock();
    g_crtc.fb_id = pf.fb_id;
    drm_unlock();

    if (wants_event) {
        /* Deliver the completion synchronously, exactly as the hardware
         * completes: the virtio-gpu command has already returned, so the
         * flip IS complete the moment the ioctl is about to return.  A
         * deferred delivery would leave the event invisible to wlroots'
         * poll until some other activity wakes its event loop, stalling
         * the frame pipeline on an idle desktop. */
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        mutex_lock(&g_vblank.lock);
        drm_vblank_deliver_due_locked();
        (void)wait_queue_collect_all(&g_vblank.waiters, 0, PROC_WAKE_EVENT,
                                     &wake_q, NULL);
        mutex_unlock(&g_vblank.lock);
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

static int drm_mode_getpropblob(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_blob b;
    if (copy_from_user(&b, arg, sizeof(b)) < 0)
        return -EFAULT;
    const uint8_t *data = NULL;
    uint32_t len = 0;
    if (b.blob_id == DRM_EDID_BLOB_ID) {
        data = drm_edid_get();
        len = 128;
    } else if (b.blob_id == DRM_IN_FORMATS_BLOB_ID) {
        data = g_in_formats_blob;
        len = sizeof(g_in_formats_blob);
    } else {
        return -ENOENT;
    }
    if (b.data && b.length >= len &&
        copy_to_user((void *)(uintptr_t)b.data, data, len) < 0)
        return -EFAULT;
    b.length = len;
    return copy_to_user(arg, &b, sizeof(b)) < 0 ? -EFAULT : 0;
}

static int drm_mode_setproperty(drm_context_t *ctx, void *arg)
{
    (void)arg;
    (void)ctx;
    return 0;
}

static int drm_mode_obj_getproperties(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_obj_get_properties o;
    if (copy_from_user(&o, arg, sizeof(o)) < 0)
        return -EFAULT;
    /* Object IDs are unique across types, so resolve purely by obj_id.  This
     * handles both specific obj_type queries and DRM_MODE_OBJECT_ANY (which
     * wlroots' get_drm_prop uses to read a property value by object ID). */
    if (o.obj_id == DRM_CONN_ID) {
        /* The connector carries a single immutable EDID blob property. */
        o.count_props = 1;
        (void)drm_edid_get();
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

static int drm_gem_create(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_gem_create c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    if (c.size == 0)
        return -EINVAL;

    drm_lock();
    drm_gem_t *g = drm_gem_alloc_locked(c.width, c.height, 0, c.bpp, c.format,
                                        0, c.size);
    if (g)
        c.handle = g->handle;
    drm_unlock();
    if (!g)
        return -ENOMEM;
    return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

static int drm_gem_mmap_ioctl(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_gem_mmap m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(m.handle);
    if (g)
        m.offset = (uint64_t)g->handle * PAGE_SIZE;
    drm_unlock();
    if (!g)
        return -ENOENT;
    /* Same encoding as MAP_DUMB, so drm_linux_mmap() serves both. */
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_gem_flink(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_gem_flink h;
    if (copy_from_user(&h, arg, sizeof(h)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(h.handle);
    if (g) {
        drm_gem_name_bind_locked(h.name, g->handle);
        h.handle = g->handle;
    }
    drm_unlock();
    if (!g)
        return -ENOENT;
    return copy_to_user(arg, &h, sizeof(h)) < 0 ? -EFAULT : 0;
}

static int drm_gem_open(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_gem_open o;
    if (copy_from_user(&o, arg, sizeof(o)) < 0)
        return -EFAULT;
    uint32_t handle = 0;
    int rc = drm_gem_name_lookup(o.name, &handle);
    if (rc < 0)
        return rc;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(handle);
    if (g)
        o.size = g->size;
    drm_unlock();
    if (!g)
        return -ENOENT;
    o.handle = handle;
    return copy_to_user(arg, &o, sizeof(o)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getfb2(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_fb_cmd2 fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    /* This used to zero the struct and report success, which made every lookup
     * of a real framebuffer look like a lookup of framebuffer 0 with no
     * geometry.  Answer it from the backing GEM like GETFB does. */
    drm_lock();
    drm_fb_t *f = drm_find_fb_locked(fb.fb_id);
    drm_gem_t *b = f ? drm_find_gem_locked(f->gem_handle) : NULL;
    memset(&fb, 0, sizeof(fb));
    if (b) {
        fb.fb_id = f->fb_id;
        fb.width = b->width;
        fb.height = b->height;
        fb.pixel_format = b->format;
        fb.pitches[0] = b->pitch;
        fb.handles[0] = b->handle;
    }
    drm_unlock();
    if (!b)
        return -ENOENT;
    /* One GEM object, linear layout, no modifier.  ADDFB2 does not accept a
     * modifier today, so reporting one here would be a lie; leave it 0. */
    fb.modifier[0] = 0;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_create_dumb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_create_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    if (d.width == 0 || d.height == 0 || d.bpp == 0)
        return -EINVAL;
    if (d.flags != 0)
        return -EINVAL;

    uint32_t pitch = ((d.width * d.bpp + 7) / 8 + 63) & ~63u;
    uint64_t size = (uint64_t)pitch * d.height;

    drm_lock();
    drm_gem_t *b = drm_gem_alloc_locked(d.width, d.height, pitch, d.bpp, 0,
                                        DRM_BO_USE_LINEAR, size);
    if (b)
        d.handle = b->handle;
    drm_unlock();
    if (!b)
        return -ENOMEM;

    d.pitch = pitch;
    d.size = size;
    return copy_to_user(arg, &d, sizeof(d)) < 0 ? -EFAULT : 0;
}

static int drm_mode_map_dumb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_map_dumb m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *b = drm_find_gem_locked(m.handle);
    if (b)
        m.offset = (uint64_t)b->handle * PAGE_SIZE;
    drm_unlock();
    if (!b)
        return -ENOENT;
    /* The Linux DRM ABI uses the fake offset handed back here as the
     * argument of a later mmap(fd, ...) call.  Return a page-aligned fake
     * offset derived from the handle; drm_linux_mmap() maps the buffer VMO
     * when the process calls mmap on /dev/dri/card0 with that offset. */
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_mode_destroy_dumb(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_destroy_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    drm_lock();
    int found = drm_find_gem_locked(d.handle) != NULL;
    drm_unlock();
    if (!found)
        return -ENOENT;
    drm_free_gem(d.handle);
    return 0;
}

static int drm_gem_close(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_gem_close c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    drm_lock();
    int found = drm_find_gem_locked(c.handle) != NULL;
    drm_unlock();
    if (!found)
        return -ENOENT;
    drm_free_gem(c.handle);
    return 0;
}

static int drm_prime_handle_to_fd(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_prime_handle p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *b = drm_find_gem_locked(p.handle);
    uint64_t bsize = b ? b->size : 0;
    struct vmo *bvmo = b ? b->vmo : NULL;
    drm_unlock();
    if (!b)
        return -ENOENT;

    int mfd = memfd_create_file(p.flags & 0x2U ? O_CLOEXEC : 0);
    if (mfd < 0)
        return mfd;

    void *snap = kmalloc(bsize);
    if (!snap) {
        vfs_close(mfd);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < (bsize + PAGE_SIZE - 1) / PAGE_SIZE; i++) {
        pfn_t pfn;
        uint64_t dst = (uint64_t)i * PAGE_SIZE;
        size_t n = PAGE_SIZE;
        if (dst + n > bsize)
            n = bsize - dst;
        if (vmo_get_page_charged(bvmo, i, NULL, &pfn) == 0)
            memcpy((uint8_t *)snap + dst, pfn_to_virt(pfn), n);
        else
            memset((uint8_t *)snap + dst, 0, n);
    }
    int r = memfd_set_contents(mfd, snap, bsize);
    kfree(snap);
    if (r < 0) {
        vfs_close(mfd);
        return r;
    }

    uint64_t id = drm_fd_identity(mfd);
    if (!id) {
        vfs_close(mfd);
        return -EIO;
    }

    /* Evict exports the user no longer holds before deciding the table is
     * full.  Liveness is resolved with the lock dropped, then the sweep only
     * touches entries whose (fd, identity) still matches what was probed, so a
     * concurrent re-export cannot be mistaken for a dead one. */
    int probe[DRM_PRIME_MAX][2];
    int nprobe = 0;
    drm_lock();
    for (int i = 0; i < g_prime_count && i < DRM_PRIME_MAX; i++) {
        probe[nprobe][0] = g_prime[i].fd;
        probe[nprobe][1] = 0;
        nprobe++;
    }
    drm_unlock();
    for (int i = 0; i < nprobe; i++)
        probe[i][1] = (int)drm_fd_identity(probe[i][0]);

    drm_lock();
    for (int i = 0; i < nprobe; ) {
        int dead = 0;
        for (int j = 0; j < nprobe; j++) {
            if (probe[j][0] == g_prime[i].fd) {
                dead = (uint64_t)probe[j][1] != g_prime[i].identity;
                break;
            }
        }
        if (dead) {
            g_prime[i] = g_prime[--g_prime_count];
            continue;
        }
        i++;
    }
    int slot = -1;
    for (int i = 0; i < g_prime_count; i++)
        if (g_prime[i].fd == mfd && g_prime[i].identity == id) {
            slot = i;
            break;
        }
    if (slot < 0 && g_prime_count < DRM_PRIME_MAX)
        slot = g_prime_count++;
    if (slot >= 0) {
        g_prime[slot].fd = mfd;
        g_prime[slot].identity = id;
        g_prime[slot].handle = p.handle;
    }
    drm_unlock();

    /* An fd that is not in the table can never be imported, so failing here is
     * the only honest outcome; returning it anyway would surface much later as
     * an unexplained ENOENT from PRIME_FD_TO_HANDLE. */
    if (slot < 0) {
        vfs_close(mfd);
        return -EMFILE;
    }

    p.fd = mfd;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_prime_fd_to_handle(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_prime_handle p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;

    uint64_t id = drm_fd_identity(p.fd);
    if (!id)
        return -EBADF;

    uint32_t handle = 0;
    int found = 0;
    drm_lock();
    for (int i = 0; i < g_prime_count; i++) {
        if (g_prime[i].identity == id) {
            handle = g_prime[i].handle;
            found = 1;
            break;
        }
    }
    drm_unlock();
    if (!found)
        return -ENOENT;
    p.handle = handle;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

/* ---- virtio-gpu 3D (DRM_IOCTL_VIRTGPU_*) ------------------------------- */

/* Create this open's host virgl context on first use.  Returns the context id,
 * or a negative errno.  Id 0 is not usable: the virtio-gpu protocol reserves
 * it, so start allocating at 1. */
static int drm_virtgpu_ensure_ctx(drm_context_t *ctx)
{
    if (ctx->virtgpu_ctx_created)
        return (int)ctx->virtgpu_ctx_id;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->ctx_create || !ops->ctx_destroy)
        return -ENODEV;

    drm_lock();
    uint32_t id = g_virtgpu_next_res++;
    if (id == 0)
        id = g_virtgpu_next_res++;
    drm_unlock();
    int rc = ops->ctx_create(drm_gpu_device(), id, 1, "a20-drm", 7);
    if (rc < 0)
        return rc;
    ctx->virtgpu_ctx_id = id;
    ctx->virtgpu_ctx_created = 1;
    return (int)id;
}

static int drm_virtgpu_getparam(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_virtgpu_getparam p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
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
            (void)ops->get_features(drm_gpu_device(), &f3d, &fctx);
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
            if (!ops->capset_info || ops->capset_info(drm_gpu_device(), idx,
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
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

/* The host addresses capsets by index, but CONTEXT_INIT names one by id, and
 * the two are not interchangeable (index 0 is capset 1, index 1 is capset 2).
 * Resolving through the host's own GET_CAPSET_INFO table is what makes the
 * client's choice mean something; assuming index == id would silently hand
 * back a different protocol than the one that was asked for. */
static uint32_t drm_virtgpu_capset_index(drm_context_t *ctx)
{
    if (!ctx->virtgpu_capset_id)
        return 0;
    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->capset_info)
        return 0;
    for (uint32_t idx = 0; idx < 16; idx++) {
        uint32_t id = 0, ver = 0, size = 0;
        if (ops->capset_info(drm_gpu_device(), idx, &id, &ver, &size) < 0)
            break;
        if (id == ctx->virtgpu_capset_id)
            return idx;
    }
    kinfo("[GPU] virtgpu: capset id %u not advertised by the host\n",
          ctx->virtgpu_capset_id);
    return 0;
}

static int drm_virtgpu_get_caps(drm_context_t *ctx, void *arg)
{
    struct drm_virtgpu_get_caps c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    if (c.size == 0 || c.size > 1024 * 1024)
        return -EINVAL;

    /* The host rejects GET_CAPSET with ERR_INVALID_PARAMETER unless the
     * request names a live context, so create this open's context first. */
    int cid = drm_virtgpu_ensure_ctx(ctx);
    if (cid < 0)
        return cid;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->get_capset)
        return -ENODEV;

    /* kcalloc, not kmalloc: the driver fills only the bytes the host actually
     * returned, and copy_to_user must not hand the caller whatever a fresh
     * heap block happened to contain past the end of the capset. */
    uint8_t *blob = kcalloc(1, c.size);
    if (!blob)
        return -ENOMEM;
    size_t got = 0;
    uint32_t index = drm_virtgpu_capset_index(ctx);
    int rc = ops->get_capset(drm_gpu_device(), (uint32_t)cid, index,
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
static int drm_gem_attach_backing(drm_gem_t *g)
{
    if (!g->vmo || g->size == 0)
        return -EINVAL;
    uint32_t res_id = g->virgl_res_id;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->resource_attach_backing)
        return -ENODEV;

    uint32_t npages = (uint32_t)((g->size + PAGE_SIZE - 1) / PAGE_SIZE);
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
        if (vmo_get_page_charged(g->vmo, i, NULL, &pfn) < 0) {
            rc = -ENOMEM;
            break;
        }
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        uint64_t len = g->size - off;
        if (len > PAGE_SIZE)
            len = PAGE_SIZE;
        entries[i].addr = (uint64_t)pfn_to_phys(pfn);
        entries[i].length = (uint32_t)len;
        entries[i].padding = 0;
    }
    if (rc == 0)
        rc = ops->resource_attach_backing(drm_gpu_device(), res_id, entries,
                                          npages);
    kfree(entries);
    if (rc == 0) {
        drm_lock();
        g->backing_attached = 1;
        drm_unlock();
    }
    return rc;
}

static int drm_virtgpu_resource_create(drm_context_t *ctx, void *arg)
{
    struct drm_virtgpu_resource_create r;
    if (copy_from_user(&r, arg, sizeof(r)) < 0)
        return -EFAULT;
    if (r.width == 0 || r.height == 0)
        return -EINVAL;

    int cid = drm_virtgpu_ensure_ctx(ctx);
    if (cid < 0)
        return cid;

    drm_gem_t *g = drm_gem_pin(r.bo_handle);
    if (!g)
        return -ENOENT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->resource_create_3d) {
        drm_gem_unpin(g);
        return -ENODEV;
    }

    drm_lock();
    uint32_t res_id = g_virtgpu_next_res++;
    if (res_id == 0)
        res_id = g_virtgpu_next_res++;
    drm_unlock();

    int rc = ops->resource_create_3d(drm_gpu_device(), (uint32_t)cid, res_id,
                                     r.target, r.format, r.bind, r.width,
                                     r.height, r.depth, r.array_size,
                                     r.last_level, r.nr_samples, r.flags);
    if (rc < 0) {
        drm_gem_unpin(g);
        return rc;
    }

    /* Promoting a buffer that is already a resource replaces the old one, so
     * the previous host resource has to be released here -- overwriting
     * virgl_res_id alone leaks it on the host for the life of the guest. */
    drm_lock();
    uint32_t old_res = g->is_virgl ? g->virgl_res_id : 0;
    g->virgl_res_id = res_id;
    g->is_virgl = 1;
    g->backing_attached = 0;
    drm_unlock();
    if (old_res && old_res != res_id && ops->resource_unref)
        ops->resource_unref(drm_gpu_device(), old_res);

    rc = drm_gem_attach_backing(g);
    if (rc < 0) {
        if (ops->resource_unref)
            ops->resource_unref(drm_gpu_device(), res_id);
        drm_lock();
        g->is_virgl = 0;
        g->virgl_res_id = 0;
        drm_unlock();
        drm_gem_unpin(g);
        return rc;
    }

    drm_gem_unpin(g);
    r.res_handle = res_id;
    return copy_to_user(arg, &r, sizeof(r)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_resource_info(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_virtgpu_resource_info i;
    if (copy_from_user(&i, arg, sizeof(i)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(i.bo_handle);
    int found = g && g->is_virgl;
    if (found) {
        i.res_handle = g->virgl_res_id;
        i.size = (uint32_t)g->size;
    }
    drm_unlock();
    if (!found)
        return -ENOENT;
    i.blob_mem = 0;
    return copy_to_user(arg, &i, sizeof(i)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_execbuffer(drm_context_t *ctx, void *arg)
{
    struct drm_virtgpu_execbuffer e;
    if (copy_from_user(&e, arg, sizeof(e)) < 0)
        return -EFAULT;
    if (e.size == 0 || e.command == 0)
        return -EINVAL;
    /* Mesa's command streams routinely exceed the small staging buffer the
     * legacy private ioctl allowed; the host accepts whatever we forward. */
    if (e.size > VIRTIO_GPU_3D_MAX_CMD_BYTES)
        return -EINVAL;

    int cid = drm_virtgpu_ensure_ctx(ctx);
    if (cid < 0)
        return cid;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    if (!ops || !ops->submit_3d)
        return -ENODEV;

    /* Publish the buffers this frame references before the stream runs.
     * RESOURCE_CREATE already backs a resource's pages once, but a GEM that
     * became a 3D resource without that step -- and any handle Mesa adds to a
     * later frame -- would reach the host with no memory mapped, so the host
     * renders into nothing.  The command stream names bo_handles, not resource
     * ids, so the kernel is the only place that can resolve them. */
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
            drm_gem_t *g = drm_gem_pin(handles[i]);
            if (!g) {
                kfree(handles);
                return -ENOENT;
            }
            drm_lock();
            int usable = g->is_virgl;
            int attached = g->backing_attached;
            drm_unlock();
            if (!usable) {
                drm_gem_unpin(g);
                kfree(handles);
                return -ENOENT;
            }
            rc = attached ? 0 : drm_gem_attach_backing(g);
            drm_gem_unpin(g);
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
    int rc = ops->submit_3d(drm_gpu_device(), (uint32_t)cid, cmd, e.size);
    kfree(cmd);
    if (rc < 0)
        return rc;

    /* We complete synchronously, so any fence the caller handed us is already
     * signalled; report success without forwarding an fd. */
    e.fence_fd = -1;
    return copy_to_user(arg, &e, sizeof(e)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_wait(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_virtgpu_3d_wait w;
    if (copy_from_user(&w, arg, sizeof(w)) < 0)
        return -EFAULT;
    /* Submission completes before the ioctl returns, so there is never
     * anything outstanding to block on.  Validating the handle still matters:
     * a blind success turns a caller that is waiting on the wrong resource
     * into a race that only shows up as corrupted pixels much later. */
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(w.handle);
    int found = g && g->is_virgl;
    drm_unlock();
    return found ? 0 : -ENOENT;
}

static int drm_virtgpu_map(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_virtgpu_map m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(m.handle);
    if (g)
        m.offset = (uint64_t)g->handle * PAGE_SIZE;
    drm_unlock();
    if (!g)
        return -ENOENT;
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_virtgpu_context_init(drm_context_t *ctx, void *arg)
{
    struct drm_virtgpu_context_init ci;
    if (copy_from_user(&ci, arg, sizeof(ci)) < 0)
        return -EFAULT;

    int cid = drm_virtgpu_ensure_ctx(ctx);
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

static int drm_virtgpu_transfer(drm_context_t *ctx, void *arg, int to_host)
{
    (void)ctx;
    struct drm_virtgpu_3d_transfer t;
    if (copy_from_user(&t, arg, sizeof(t)) < 0)
        return -EFAULT;
    drm_lock();
    drm_gem_t *g = drm_find_gem_locked(t.bo_handle);
    int found = g && g->is_virgl;
    drm_unlock();
    if (!found)
        return -ENOENT;

    /* Transfers are driven by the host as part of rendering; with a
     * synchronously-completing submit the backing is already coherent.  Keep
     * the ioctl as a validated no-op rather than an EINVAL hole. */
    (void)to_host;
    return 0;
}

/* ---- vfile backend ---- */

static int drm_read(vfile_t *vf, char *buf, size_t count)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx || !buf)
        return -EBADF;
    if (count < 32)
        return -EINVAL;

    drm_vblank_init_once();
    for (;;) {
        mutex_lock(&g_vblank.lock);
        drm_vblank_deliver_due_locked();
        if (ctx->ev_head != ctx->ev_tail) {
            /* libdrm reads with a large buffer and handles several events
             * per read; drain as many complete records as fit. */
            size_t n = 0;
            while (ctx->ev_head != ctx->ev_tail && n + 32 <= count) {
                memcpy(buf + n, ctx->events[ctx->ev_head], 32);
                ctx->ev_head = (ctx->ev_head + 1) % DRM_CTX_EVENT_MAX;
                n += 32;
            }
            mutex_unlock(&g_vblank.lock);
            return (int)n;
        }
        uint64_t deadline = 0;
        mutex_unlock(&g_vblank.lock);

        if (vf->flags & O_NONBLOCK)
            return -EAGAIN;

        /* Block until a future page flip posts an event to this file and
         * wakes the queue. */
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&g_vblank.waiters, &entry, token, 0);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&g_vblank.waiters, &entry);
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

static int drm_poll(vfile_t *vf, short events)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return POLLNVAL;
    short revents = 0;
    /* A page-flip completion becomes visible at its vblank deadline; report
     * the fd readable only while a completed event is actually queued, or
     * libdrm spins in drmHandleEvent() on an empty fd. */
    drm_vblank_init_once();
    mutex_lock(&g_vblank.lock);
    drm_vblank_deliver_due_locked();
    if ((events & POLLIN) && ctx->ev_head != ctx->ev_tail)
        revents |= POLLIN;
    mutex_unlock(&g_vblank.lock);
    return revents;
}

static size_t drm_poll_sources(vfile_t *vf, short events,
                               readiness_source_t *sources, size_t max)
{
    (void)vf;
    if (!sources || max == 0 || !(events & POLLIN))
        return 0;
    drm_vblank_init_once();
    sources[0] = (readiness_source_t){ &g_vblank.waiters, 0, 0 };
    return 1;
}

static int drm_close(vfile_t *vf)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (ctx) {
        /* Buffers are global; they are reclaimed by DESTROY_DUMB / GEM_CLOSE.
         * Do not free them on close (a compositor may close the allocator fd
         * while buffers are still referenced by the backend). */
        drm_vblank_init_once();
        mutex_lock(&g_vblank.lock);
        if (g_vblank.flip_ctx == ctx) {
            g_vblank.flip_pending = 0;
            g_vblank.flip_ctx = NULL;
        }
        mutex_unlock(&g_vblank.lock);

        /* Releasing g_vblank.lock first is deliberate: g_drm.lock is documented
         * as never held together with it, and this is the one path that needs
         * both.  g_vblank.flip_ctx can no longer name this context here, so no
         * queued event can be delivered into freed storage. */
        uint32_t cid = ctx->virtgpu_ctx_created ? ctx->virtgpu_ctx_id : 0;
        ctx->virtgpu_ctx_created = 0;
        if (cid) {
            gpu_dev_ops_t *ops = drm_gpu_ops();
            if (ops && ops->ctx_destroy)
                ops->ctx_destroy(drm_gpu_device(), cid);
        }
        kfree(ctx);
        vf->priv = NULL;
    }
    return 0;
}

static int drm_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return -EBADF;

    switch (req) {
    case DRM_IOCTL_VERSION:
        return drm_version(ctx, arg);
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
        return drm_gem_close(ctx, arg);
    case A20_GPU_IOCTL_GEM_CREATE:
        return drm_gem_create(ctx, arg);
    case A20_GPU_IOCTL_GEM_MMAP:
        return drm_gem_mmap_ioctl(ctx, arg);
    case DRM_IOCTL_GEM_FLINK:
        return drm_gem_flink(ctx, arg);
    case DRM_IOCTL_GEM_OPEN:
        return drm_gem_open(ctx, arg);
    case DRM_IOCTL_VIRTGPU_GETPARAM:
        return drm_virtgpu_getparam(ctx, arg);
    case DRM_IOCTL_VIRTGPU_GET_CAPS:
        return drm_virtgpu_get_caps(ctx, arg);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return drm_virtgpu_resource_create(ctx, arg);
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO:
        return drm_virtgpu_resource_info(ctx, arg);
    case DRM_IOCTL_VIRTGPU_EXECBUFFER:
        return drm_virtgpu_execbuffer(ctx, arg);
    case DRM_IOCTL_VIRTGPU_WAIT:
        return drm_virtgpu_wait(ctx, arg);
    case DRM_IOCTL_VIRTGPU_MAP:
        return drm_virtgpu_map(ctx, arg);
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT:
        return drm_virtgpu_context_init(ctx, arg);
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return drm_virtgpu_transfer(ctx, arg, 1);
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return drm_virtgpu_transfer(ctx, arg, 0);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        return drm_prime_handle_to_fd(ctx, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return drm_prime_fd_to_handle(ctx, arg);
    case DRM_IOCTL_MODE_GETRESOURCES:
        return drm_mode_getresources(ctx, arg);
    case DRM_IOCTL_MODE_GETCRTC:
        return drm_mode_getcrtc(ctx, arg);
    case DRM_IOCTL_MODE_SETCRTC:
        return drm_mode_setcrtc(ctx, arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return drm_mode_getconnector(ctx, arg);
    case DRM_IOCTL_MODE_GETENCODER:
        return drm_mode_getencoder(ctx, arg);
    case DRM_IOCTL_MODE_GETPLANE:
        return drm_mode_getplane(ctx, arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        return drm_mode_getplaneres(ctx, arg);
    case DRM_IOCTL_MODE_GETFB:
        return drm_mode_getfb(ctx, arg);
    case DRM_IOCTL_MODE_GETFB2:
        return drm_mode_getfb2(ctx, arg);
    case DRM_IOCTL_MODE_ADDFB:
        return drm_mode_addfb(ctx, arg);
    case DRM_IOCTL_MODE_ADDFB2:
        return drm_mode_addfb2(ctx, arg);
    case DRM_IOCTL_MODE_RMFB:
        return drm_mode_rmfb(ctx, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return drm_mode_pageflip(ctx, arg);
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
        return drm_mode_getpropblob(ctx, arg);
    case DRM_IOCTL_MODE_SETPROPERTY:
        return drm_mode_setproperty(ctx, arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return drm_mode_obj_getproperties(ctx, arg);
    case DRM_IOCTL_MODE_ATOMIC:
        return drm_mode_atomic(ctx, arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return drm_mode_create_dumb(ctx, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
        return drm_mode_map_dumb(ctx, arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB:
        return drm_mode_destroy_dumb(ctx, arg);
    default:
        /* The A20-private 3D transport ioctls used to be forwarded to the GPU
         * driver from here.  They are gone: every one of them is covered by
         * the upstream VIRTGPU UAPI above, and keeping a second 3D ABI meant
         * two paths that can drift while only the upstream one is the one Mesa
         * ever speaks.  Unknown requests stay EINVAL. */
        return -EINVAL;
    }
}

static vfile_ops_t g_drm_ops = {
    .read = drm_read,
    .write = drm_write,
    .lseek = drm_lseek,
    .ioctl = drm_ioctl,
    .poll = drm_poll,
    .poll_sources = drm_poll_sources,
    .close = drm_close,
};

int drm_create_file(void)
{
    vfile_t *vf = drm_create_vfile();
    if (!vf)
        return -ENOMEM;
    return anonfd_install_vfile(vf, 0);
}

/* Create a DRM vfile without installing it into the fd table (used by the
 * /dev/dri/card0 devfs open path, which manages fd installation itself). */
vfile_t *drm_create_vfile(void)
{
    drm_context_t *ctx = kcalloc(1, sizeof(*ctx));
    vfile_t *vf = vfile_alloc();
    if (!ctx || !vf) {
        if (ctx) kfree(ctx);
        if (vf) vfile_free(vf);
        return NULL;
    }
    vfile_ref_init(vf, 1);
    vf->flags = O_RDWR;
    vf->ops = &g_drm_ops;
    vf->priv = ctx;
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

int64_t drm_linux_mmap(vfile_t *vf, uint64_t addr, size_t len, int prot,
                       int flags, uint64_t off)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return -EBADF;

    drm_gem_t *b = drm_gem_pin((uint32_t)(off / PAGE_SIZE));
    if (!b)
        return -ENOENT;

    task_t *t = proc_current();
    if (!t || !t->mm) {
        drm_gem_unpin(b);
        return -EFAULT;
    }

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0 || map_len > b->size) {
        drm_gem_unpin(b);
        return -EINVAL;
    }

    uint64_t map_addr = mm_mmap_vmo(t->mm, addr, map_len, prot, flags,
                                    b->vmo, 0);
    drm_gem_unpin(b);
    if (map_addr == 0 || mm_addr_is_error((vaddr_t)map_addr)) {
        return -ENOMEM;
    }
    return (int64_t)map_addr;
}
