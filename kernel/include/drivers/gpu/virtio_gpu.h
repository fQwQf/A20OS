#ifndef _VIRTIO_GPU_H
#define _VIRTIO_GPU_H

#include "core/types.h"
#include "drivers/core/driver_class.h"

/* VirtIO GPU Feature bits */
#define VIRTIO_GPU_F_VIRGL               0
#define VIRTIO_GPU_F_EDID                1
#define VIRTIO_GPU_F_CONTEXT_INIT        4
#define VIRTIO_GPU_F_RESOURCE_UUID       2

/* VirtIO GPU Control commands (2D) */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF         0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

/* VirtIO GPU Control commands (3D / context / blob) */
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_GET_EDID                 0x010a
#define VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID     0x010b
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB     0x010c
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB         0x010d
#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB        0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB      0x0209

/* VirtIO GPU Success responses */
#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO       0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET            0x1103
#define VIRTIO_GPU_RESP_OK_EDID              0x1104
#define VIRTIO_GPU_RESP_OK_RESOURCE_UUID     0x1105
#define VIRTIO_GPU_RESP_OK_MAP_INFO          0x1106

/* VirtIO GPU error responses */
#define VIRTIO_GPU_RESP_ERR_UNSPEC           0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY    0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

/* Capset IDs */
#define VIRTIO_GPU_CAPSET_VIRGL      1
#define VIRTIO_GPU_CAPSET_VIRGL2     2
#define VIRTIO_GPU_CAPSET_VENUS      4
#define VIRTIO_GPU_CAPSET_DRM        6

/* The DRM capset is answered by the kernel, not by the host.
 *
 * Every other capset id names a renderer-side feature set that virglrenderer
 * (or Venus) produces and that GET_CAPSET_INFO therefore reports on. Capset 6 is
 * different: it describes what the *kernel's own* virtio-gpu driver supports,
 * and Linux answers it in virtio_gpu_ioctl_get_caps() without ever reaching the
 * host. A client that asks for it is asking "what can this driver do", so
 * forwarding it to virglrenderer returns a renderer capset under a
 * driver-capset id, which the client then rejects.
 *
 * Layout matches the Linux UAPI struct virtio_gpu_drm_caps, which is the prefix
 * of the client's own capset union -- so it must be written at offset 0 of the
 * supplied buffer. */
struct virtio_gpu_drm_caps {
    uint64_t caps_set;
    uint32_t max_version;
    uint32_t reserved[2];
} __attribute__((packed));

/* Formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM     1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM     2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM     3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM     4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM     67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM     68

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_get_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t enabled;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

#define VIRTIO_GPU_EDID_MAX_BYTES 1024

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t size;
    uint32_t padding;
    uint8_t edid[VIRTIO_GPU_EDID_MAX_BYTES];
} __attribute__((packed));

struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed));

/* capset_id is an *id*, not an index into GET_CAPSET_INFO's list.  The two are
 * easy to confuse because GET_CAPSET_INFO takes an index while GET_CAPSET takes
 * an id, and mixing them fails in a way that looks like a broken host: QEMU
 * hands this field straight to virgl_renderer_get_cap_set(), whose default arm
 * reports a zero-length capset for an unknown id, and QEMU turns that into
 * VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER (0x1205).  Index 0 is capset id 1, so
 * sending the index answers a question nobody asked.  The wire layout is a pair
 * of little-endian u32 either way, so this rename is documentation, not ABI. */
struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} __attribute__((packed));

struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_data[];
} __attribute__((packed));

#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP (1 << 0)

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
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
    uint32_t padding;
} __attribute__((packed));

#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK 0x000000ff

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

/* Byte-identical to virtio_gpu_transfer_to_host_3d; the spec spells out both
 * directions separately and only the command code tells them apart.  This is
 * the one that makes a rendered resource readable by the guest: without it the
 * host keeps the result in its own GL object, and reading the guest's page
 * returns whatever was there before the submit. */
struct virtio_gpu_transfer_from_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

#define VIRTIO_GPU_BLOB_MEM_GUEST  0x0001
#define VIRTIO_GPU_BLOB_MEM_HOST3D 0x0002
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST 0x0003
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE 0x0001
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004

struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t nr_entries;
    uint64_t size;
    uint64_t uuid_lo;
    uint64_t uuid_hi;
} __attribute__((packed));

struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
    uint64_t offset;
} __attribute__((packed));

struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t map_info;
    uint32_t padding;
} __attribute__((packed));

struct device;
struct device *virtio_gpu_get_dev(void);

/* Upper bound on a 3D command stream.  Shared by the DRM VIRTGPU layer and
 * the driver so the two cannot disagree: if the ioctl accepts a size the
 * driver then rejects, a large Mesa command stream fails deep in the driver
 * with -EINVAL instead of at the call that caused it.  The submit path stages
 * the blob in a buffer it grows on demand, so this is a sanity limit, not a
 * structural one. */
#define VIRTIO_GPU_3D_MAX_CMD_BYTES (16u * 1024u * 1024u)

/* Ceiling on a single capset blob.  Capsets are a few kilobytes today, but
 * the host owns this number and DRM_IOCTL_VIRTGPU_GET_CAPS already admits
 * 1 MiB from userspace; this bounds the driver's own staging allocation so a
 * bad host answer cannot turn a capability query into an unbounded kmalloc. */
#define VIRTIO_GPU_MAX_CAPSET_BYTES (1u * 1024u * 1024u)


/* A20 3D passthrough request payload.  The submit path points cmdbuf at
 * user memory holding a virgl command stream. */

#endif
