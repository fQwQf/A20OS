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
#define DRM_CTX_EVENT_MAX 16

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

/* ---- connector EDID ---- */

#define DRM_EDID_PROP_ID 1u
#define DRM_EDID_BLOB_ID 1u

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
    if (!g_edid_ready) {
        int done = 0;
        gpu_dev_ops_t *ops = drm_gpu_ops();
        if (ops && ops->get_edid) {
            uint8_t buf[128];
            int n = ops->get_edid(drm_gpu_device(), buf, sizeof(buf));
            if (n >= 128 && drm_edid_valid(buf)) {
                memcpy(g_edid, buf, sizeof(g_edid));
                done = 1;
            }
        }
        if (!done) {
            uint32_t w = 1024, h = 768, bpp = 32;
            if (ops && ops->get_info)
                (void)ops->get_info(drm_gpu_device(), &w, &h, &bpp);
            drm_edid_synthesize(g_edid, w, h);
        }
        g_edid_ready = 1;
    }
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
    con.count_props = 1;
    con.count_encoders = 1;

    (void)drm_edid_get();
    if (con.modes_ptr && copy_to_user((void *)(uintptr_t)con.modes_ptr,
                                      &mode, sizeof(mode)) < 0)
        return -EFAULT;
    if (con.encoders_ptr && copy_to_user((void *)(uintptr_t)con.encoders_ptr,
                                         &(uint32_t){ 1 }, sizeof(uint32_t)) < 0)
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
    struct drm_mode_crtc_page_flip pf;
    if (copy_from_user(&pf, arg, sizeof(pf)) < 0)
        return -EFAULT;
    drm_buffer_t *b = drm_find_buffer(ctx, pf.fb_id);
    if (!b)
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

    if (drm_present_buffer(b) < 0) {
        if (wants_event) {
            mutex_lock(&g_vblank.lock);
            g_vblank.flip_pending = 0;
            g_vblank.flip_ctx = NULL;
            mutex_unlock(&g_vblank.lock);
        }
        return -EIO;
    }

    if (wants_event) {
        /* Wake any blocking readers so they can re-park on the flip's
         * vblank deadline instead of sleeping without one. */
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        mutex_lock(&g_vblank.lock);
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
    if (p.prop_id != DRM_EDID_PROP_ID)
        return -ENOENT;
    memset(p.name, 0, sizeof(p.name));
    strncpy(p.name, "EDID", sizeof(p.name) - 1);
    p.flags = 0x10 | 0x04; /* DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE */
    p.count_values = 1;
    p.count_enum_blobs = 0;
    if (p.values_ptr && copy_to_user((void *)(uintptr_t)p.values_ptr,
                                     &(uint64_t){ DRM_EDID_BLOB_ID },
                                     sizeof(uint64_t)) < 0)
        return -EFAULT;
    return copy_to_user(arg, &p, sizeof(p)) < 0 ? -EFAULT : 0;
}

static int drm_mode_getpropblob(drm_context_t *ctx, void *arg)
{
    (void)ctx;
    struct drm_mode_get_blob b;
    if (copy_from_user(&b, arg, sizeof(b)) < 0)
        return -EFAULT;
    if (b.blob_id != DRM_EDID_BLOB_ID)
        return -ENOENT;
    const uint8_t *edid = drm_edid_get();
    if (b.data && b.length >= 128 &&
        copy_to_user((void *)(uintptr_t)b.data, edid, 128) < 0)
        return -EFAULT;
    b.length = 128;
    return copy_to_user(arg, &b, sizeof(b)) < 0 ? -EFAULT : 0;
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
    /* The connector carries a single immutable EDID blob property (wlroots
     * discovers it via object properties); other objects expose none.
     * DRM_MODE_OBJECT_ANY (0) queries must also match: wlroots reads the
     * current blob-id value that way. */
    if ((o.obj_type == 0xc0c0c0c0 || o.obj_type == 0) &&
        o.obj_id == 1) { /* DRM_MODE_OBJECT_CONNECTOR / DRM_MODE_OBJECT_ANY */
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
