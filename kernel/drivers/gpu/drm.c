#include "drivers/gpu/drm.h"
#include "drivers/gpu/virtio_gpu.h"

#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/poll.h"
#include "drivers/core/driver_class.h"
#include "drivers/gpu/gpu_core.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/memfd.h"
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

#define DRM_MAX_BUFFERS 16
#define DRM_EVENT_FLIP_COMPLETE 0x02

typedef struct drm_buffer {
    int used;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    struct vmo *vmo;
    uint64_t size;
} drm_buffer_t;

typedef struct drm_context {
    uint32_t next_handle;
    uint32_t magic;
    int is_master;
    uint8_t event[32];
    size_t event_len;
} drm_context_t;

/* DRM dumb buffers are global to the device, so a handle created by one open
 * (the wlroots dumb allocator) is visible to another (the wlroots backend),
 * matching Linux DRM semantics.  wlroots exports a dumb buffer via PRIME on
 * one fd and imports it on another, then creates an FB and pages it in. */
static drm_buffer_t g_buffers[DRM_MAX_BUFFERS];
static int g_buffer_count;

/* PRIME fd <-> GEM handle mapping.  The dumb allocator exports a buffer
 * through one DRM open and the backend imports it through another, so the
 * mapping must be global, not per-context. */
#define DRM_PRIME_MAX 64
static int g_prime_fd[DRM_PRIME_MAX];
static uint32_t g_prime_handle[DRM_PRIME_MAX];
static int g_prime_count;

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
 */
static int drm_present_buffer(drm_buffer_t *b)
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
        bpp != 32 || b->width != width || b->height != height ||
        b->pitch < width * 4 || fb_size < (size_t)height * width * 4)
        return -EINVAL;

    uint8_t *dst = (uint8_t *)pfn_to_virt(phys_to_pfn(fb_phys));
    if (!dst)
        return -EFAULT;

    for (uint32_t y = 0; y < height; y++) {
        size_t src_offset = (size_t)y * b->pitch;
        size_t dst_offset = (size_t)y * width * 4;
        size_t remaining = (size_t)width * 4;
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

    int ret = ops->flush(dev, 0, 0, width, height);
    if (present_count < 4) {
        kinfo("[DRM] present handle=%u pages=%lu flush=%d\n",
              b->handle, (unsigned long)((b->size + PAGE_SIZE - 1) / PAGE_SIZE),
              ret);
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

/* ---- helpers ---- */

static drm_buffer_t *drm_find_buffer(drm_context_t *ctx, uint32_t handle)
{
    (void)ctx;
    for (int i = 0; i < g_buffer_count; i++)
        if (g_buffers[i].used && g_buffers[i].handle == handle)
            return &g_buffers[i];
    return NULL;
}

static void drm_free_buffer(drm_context_t *ctx, drm_buffer_t *b)
{
    (void)ctx;
    if (b->vmo)
        vmo_release(b->vmo);
    memset(b, 0, sizeof(*b));
    b->used = 0;
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
    case 0x5: /* DRM_CAP_PRIME */
        c.value = 0;
        break;
    case 0x6: /* DRM_CAP_TIMESTAMP_MONOTONIC */
        c.value = 1;
        break;
    case 0x7: /* DRM_CAP_ASYNC_PAGE_FLIP */
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
        ctx->magic = (uint32_t)ctx + 0xA20; /* arbitrary per-open magic */
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
    struct drm_mode_card_res res;
    if (copy_from_user(&res, arg, sizeof(res)) < 0)
        return -EFAULT;

    gpu_dev_ops_t *ops = drm_gpu_ops();
    uint32_t w = 1024, h = 768, bpp = 32;
    if (ops && ops->get_info) {
        struct device *dev = drm_gpu_device();
        (void)ops->get_info(dev, &w, &h, &bpp);
    }

    int nfbs = 0;
    for (int i = 0; i < g_buffer_count; i++)
        if (g_buffers[i].used)
            nfbs++;

    uint32_t fbs[DRM_MAX_BUFFERS];
    uint32_t crtcs[1] = { 1 };
    uint32_t conns[1] = { 1 };
    uint32_t encs[1] = { 1 };
    int fi = 0;
    for (int i = 0; i < g_buffer_count; i++)
        if (g_buffers[i].used)
            fbs[fi++] = g_buffers[i].handle;

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
    c.crtc_id = 1;
    c.mode_valid = 1;
    c.gamma_size = 0;
    drm_mode_fill(&c.mode, w, h, 60);
    return copy_to_user(arg, &c, sizeof(c)) < 0 ? -EFAULT : 0;
}

static int drm_mode_setcrtc(drm_context_t *ctx, void *arg)
{
    struct drm_mode_crtc c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    if (c.fb_id != 0) {
        drm_buffer_t *b = drm_find_buffer(ctx, c.fb_id);
        if (!b)
            return -ENOENT;
        /* The minimal KMS implementation presents by copying into the GPU's
         * primary scanout resource before issuing TRANSFER_TO_HOST_2D. */
        if (drm_present_buffer(b) < 0)
            return -EIO;
    }
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

    con.connector_id = 1;
    con.connector_type = 15; /* DRM_MODE_CONNECTOR_VIRTUAL */
    con.connector_type_id = 1;
    con.connection = 1;      /* connected */
    con.mm_width = 0;
    con.mm_height = 0;
    con.subpixel = 0;
    con.encoder_id = 1;
    con.count_modes = 1;
    con.count_props = 0;
    con.count_encoders = 1;

    if (con.modes_ptr && copy_to_user((void *)(uintptr_t)con.modes_ptr,
                                      &mode, sizeof(mode)) < 0)
        return -EFAULT;
    if (con.encoders_ptr && copy_to_user((void *)(uintptr_t)con.encoders_ptr,
                                         &(uint32_t){ 1 }, sizeof(uint32_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &con, sizeof(con)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getencoder(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_encoder e;
    if (copy_from_user(&e, arg, sizeof(e)) < 0)
        return -EFAULT;
    e.encoder_id = 1;
    e.encoder_type = 4; /* DRM_MODE_ENCODER_VIRTUAL */
    e.crtc_id = 1;
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
    p.plane_id = 1;
    p.crtc_id = 1;
    p.fb_id = 0;
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
                                       &(uint32_t){ 1 }, sizeof(uint32_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &r, sizeof(r)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getfb(drm_context_t *ctx, void *arg)
{
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, fb.fb_id);
    if (!b)
        return -ENOENT;
    fb.width = b->width;
    fb.height = b->height;
    fb.pitch = b->pitch;
    fb.bpp = b->bpp;
    fb.depth = 24;
    fb.handle = b->handle;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_addfb(drm_context_t *ctx, void *arg)
{
    struct drm_mode_fb_cmd fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, fb.handle);
    if (!b)
        return -ENOENT;
    fb.fb_id = b->handle;
    fb.pitch = b->pitch;
    fb.bpp = b->bpp;
    fb.depth = 24;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_addfb2(drm_context_t *ctx, void *arg)
{
    struct drm_mode_fb_cmd2 fb;
    if (copy_from_user(&fb, arg, sizeof(fb)) < 0)
        return -EFAULT;
    if (fb.handles[0] == 0)
        return -EINVAL;
    drm_buffer_t *b = drm_find_buffer(ctx, fb.handles[0]);
    if (!b)
        return -ENOENT;
    fb.fb_id = b->handle;
    fb.pitches[0] = b->pitch;
    return copy_to_user(arg, &fb, sizeof(fb)) < 0 ? -EFAULT : 0;
}

static int drm_mode_rmfb(drm_context_t *ctx, void *arg)
{
    uint32_t fb_id = 0;
    if (copy_from_user(&fb_id, arg, sizeof(fb_id)) < 0)
        return -EFAULT;
    /* Keep the dumb buffer alive (the handle still owns it); just drop the
     * framebuffer id association, which is the same id here. */
    return 0;
}

static int drm_mode_pageflip(drm_context_t *ctx, void *arg)
{
    static unsigned int pageflip_count;
    struct drm_mode_crtc_page_flip pf;
    if (copy_from_user(&pf, arg, sizeof(pf)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, pf.fb_id);
    if (!b)
        return -ENOENT;
    if (drm_present_buffer(b) < 0)
        return -EIO;
    if (pf.flags & 0x1) { /* DRM_MODE_PAGE_FLIP_EVENT */
        struct drm_event_vblank event;
        memset(&event, 0, sizeof(event));
        event.type = DRM_EVENT_FLIP_COMPLETE;
        event.length = sizeof(event);
        event.user_data = pf.user_data;
        event.sequence = 0;
        event.crtc_id = pf.crtc_id;
        memcpy(ctx->event, &event, sizeof(event));
        ctx->event_len = sizeof(event);
    }
    if (pageflip_count < 4) {
        kinfo("[DRM] pageflip flags=%x event_len=%lu\n",
              pf.flags, (unsigned long)ctx->event_len);
        pageflip_count++;
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
    (void)ctx;
    /* A20OS virtio-gpu has no hardware cursor plane; accept the request so
     * wlroots' legacy page-flip path can proceed. */
    return 0;
}

static int drm_mode_cursor2(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    return 0;
}

static int drm_mode_getgamma(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    return 0;
}

static int drm_mode_getproperty(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_property p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    /* Report no properties for now. */
    p.count_values = 0;
    p.count_enum_blobs = 0;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_mode_setproperty(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    return 0;
}

static int drm_mode_obj_getproperties(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_obj_get_properties o;
    if (copy_from_user(&o, arg, sizeof(o)) < 0)
        return -EFAULT;
    /* A20OS exposes no KMS object properties.  Report an empty set for any
     * valid object id.  (wlroots queries per-object properties to build its
     * atomic state; an empty set is valid for a minimal display.) */
    o.count_props = 0;
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

static int drm_mode_create_dumb(drm_context_t *ctx, void *arg)
{
    struct drm_mode_create_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    if (d.width == 0 || d.height == 0 || d.bpp == 0)
        return -EINVAL;
    if (d.flags != 0)
        return -EINVAL;

    int slot = -1;
    for (int i = 0; i < DRM_MAX_BUFFERS; i++) {
        if (!g_buffers[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -ENOMEM;

    uint32_t pitch = ((d.width * d.bpp + 7) / 8 + 63) & ~63u;
    uint64_t size = (uint64_t)pitch * d.height;
    struct vmo *vmo = vmo_create(VMO_ANONYMOUS, size, 0);
    if (!vmo)
        return -ENOMEM;

    drm_buffer_t *b = &g_buffers[slot];
    b->used = 1;
    b->handle = ctx->next_handle++;
    if (b->handle == 0)
        b->handle = ctx->next_handle++;
    b->width = d.width;
    b->height = d.height;
    b->pitch = pitch;
    b->bpp = d.bpp;
    b->vmo = vmo;
    if (slot + 1 > g_buffer_count)
        g_buffer_count = slot + 1;
    b->size = size;

    d.handle = b->handle;
    d.pitch = pitch;
    d.size = size;
    return copy_to_user(arg, &d, sizeof(d)) < 0 ? -EFAULT : 0;
}

static int drm_mode_map_dumb(drm_context_t *ctx, void *arg)
{
    struct drm_mode_map_dumb m;
    if (copy_from_user(&m, arg, sizeof(m)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, m.handle);
    if (!b)
        return -ENOENT;
    /* The Linux DRM ABI uses the fake offset handed back here as the
     * argument of a later mmap(fd, ...) call.  Return a page-aligned fake
     * offset derived from the handle; drm_linux_mmap() maps the buffer VMO
     * when the process calls mmap on /dev/dri/card0 with that offset. */
    m.offset = (uint64_t)b->handle * PAGE_SIZE;
    return copy_to_user(arg, &m, sizeof(m)) < 0 ? -EFAULT : 0;
}

static int drm_mode_destroy_dumb(drm_context_t *ctx, void *arg)
{
    struct drm_mode_destroy_dumb d;
    if (copy_from_user(&d, arg, sizeof(d)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, d.handle);
    if (!b)
        return -ENOENT;
    drm_free_buffer(ctx, b);
    return 0;
}

static int drm_gem_close(drm_context_t *ctx, void *arg)
{
    struct drm_gem_close c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, c.handle);
    if (!b)
        return -ENOENT;
    /* Keep the dumb buffer alive; the handle owns the VMO and is dropped by
     * DRM_IOCTL_MODE_DESTROY_DUMB.  GEM close is a no-op for our handles. */
    return 0;
}

static int drm_prime_handle_to_fd(drm_context_t *ctx, void *arg)
{
    struct drm_prime_handle p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, p.handle);
    if (!b)
        return -ENOENT;

    int mfd = memfd_create_file(p.flags & 0x2U ? O_CLOEXEC : 0);
    if (mfd < 0)
        return mfd;

    void *snap = kmalloc(b->size);
    if (!snap) {
        vfs_close(mfd);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < (b->size + PAGE_SIZE - 1) / PAGE_SIZE; i++) {
        pfn_t pfn;
        uint64_t dst = (uint64_t)i * PAGE_SIZE;
        size_t n = PAGE_SIZE;
        if (dst + n > b->size)
            n = b->size - dst;
        if (vmo_get_page_charged(b->vmo, i, NULL, &pfn) == 0)
            memcpy((uint8_t *)snap + dst, pfn_to_virt(pfn), n);
        else
            memset((uint8_t *)snap + dst, 0, n);
    }
    int r = memfd_set_contents(mfd, snap, b->size);
    kfree(snap);
    if (r < 0) {
        vfs_close(mfd);
        return r;
    }

    if (g_prime_count < DRM_PRIME_MAX) {
        g_prime_fd[g_prime_count] = mfd;
        g_prime_handle[g_prime_count] = b->handle;
        g_prime_count++;
    }

    p.fd = mfd;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_prime_fd_to_handle(drm_context_t *ctx, void *arg)
{
    struct drm_prime_handle p;
    if (copy_from_user(&p, arg, sizeof(p)) < 0)
        return -EFAULT;
    for (int i = 0; i < g_prime_count; i++) {
        if (g_prime_fd[i] == p.fd) {
            p.handle = g_prime_handle[i];
            return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
        }
    }
    return -ENOENT;
}

/* ---- vfile backend ---- */

static int drm_read(vfile_t *vf, char *buf, size_t count)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (!ctx || !buf)
        return -EBADF;
    if (ctx->event_len == 0)
        return (vf->flags & O_NONBLOCK) ? -EAGAIN : -EAGAIN;
    if (count < ctx->event_len)
        return -EINVAL;
    memcpy(buf, ctx->event, ctx->event_len);
    int ret = (int)ctx->event_len;
    ctx->event_len = 0;
    kinfo("[DRM] read event bytes=%d\n", ret);
    return ret;
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
    /* KMS commits are acknowledged synchronously by virtio-gpu.  A page-flip
     * ioctl queues its completion before wlroots returns to poll(), so report
     * the fd readable only while that completion is pending.  Advertising
     * unconditional readability makes libdrm call drmHandleEvent() on an
     * empty fd forever, starving Wayland clients and input dispatch. */
    if ((events & POLLIN) && ctx->event_len != 0)
        revents |= POLLIN;
    return revents;
}

static int drm_close(vfile_t *vf)
{
    drm_context_t *ctx = vf ? vf->priv : NULL;
    if (ctx) {
        /* Buffers are global; they are reclaimed by DESTROY_DUMB / GEM_CLOSE.
         * Do not free them on close (a compositor may close the allocator fd
         * while buffers are still referenced by the backend). */
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
    case DRM_IOCTL_GEM_CLOSE:
        return drm_gem_close(ctx, arg);
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
        /* A20 virtio-gpu 3D passthrough: forward the request to the GPU
         * driver's gpu_dev_ops_t.ioctl.  The per-open DRM context is not
         * involved; the GPU driver owns the virgl context/resource id
         * space and the controlq submission. */
        if (req >= A20_GPU_IOCTL_BASE && req < A20_GPU_IOCTL_BASE + 16) {
            gpu_dev_ops_t *ops = drm_gpu_ops();
            if (!ops || !ops->ioctl)
                return -ENODEV;
            return ops->ioctl(drm_gpu_device(), req, arg);
        }
        return -EINVAL;
    }
}

static vfile_ops_t g_drm_ops = {
    .read = drm_read,
    .write = drm_write,
    .lseek = drm_lseek,
    .ioctl = drm_ioctl,
    .poll = drm_poll,
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
    ctx->next_handle = 1;
    vfile_ref_init(vf, 1);
    vf->flags = O_RDWR;
    vf->ops = &g_drm_ops;
    vf->priv = ctx;
    return vf;
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

    drm_buffer_t *b = drm_find_buffer(ctx, (uint32_t)(off / PAGE_SIZE));
    if (!b)
        return -ENOENT;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EFAULT;

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) {
        return -EINVAL;
    }
    if (map_len > b->size) {
        return -EINVAL;
    }

    uint64_t map_addr = mm_mmap_vmo(t->mm, addr, map_len, prot, flags,
                                    b->vmo, 0);
    if (map_addr == 0 || (int64_t)map_addr < 0) {
        return -ENOMEM;
    }
    return (int64_t)map_addr;
}
