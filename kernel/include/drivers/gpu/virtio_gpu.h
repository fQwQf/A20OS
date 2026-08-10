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

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
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

/* ---- A20 virtio-gpu 3D passthrough ioctls ----
 * Exposed through gpu_dev_ops_t.ioctl (req = _IOW('G', nr, size) style
 * codes local to the A20 GPU layer).  User space (the virgl client stack)
 * uses these to drive the host-side GL context over the virtio-gpu 3D
 * controlq.  arg points to a virtio_gpu_3d_req for all of them. */
#define A20_GPU_IOCTL_BASE  0x4700UL   /* 'G' family, A20 private */
#define A20_GPU_IOCTL_CTX_CREATE     (A20_GPU_IOCTL_BASE + 1)
#define A20_GPU_IOCTL_CTX_DESTROY    (A20_GPU_IOCTL_BASE + 2)
#define A20_GPU_IOCTL_RES_CREATE_3D  (A20_GPU_IOCTL_BASE + 3)
#define A20_GPU_IOCTL_RES_UNREF      (A20_GPU_IOCTL_BASE + 4)
#define A20_GPU_IOCTL_SUBMIT_3D      (A20_GPU_IOCTL_BASE + 5)
#define A20_GPU_IOCTL_VIRGL_CHECK    (A20_GPU_IOCTL_BASE + 6)

/* A20 3D passthrough request payload.  The submit path points cmdbuf at
 * user memory holding a virgl command stream. */
struct virtio_gpu_3d_req {
    uint32_t ctx_id;
    uint32_t resource_id;
    uint32_t target;        /* create_3d */
    uint32_t format;        /* create_3d */
    uint32_t bind;          /* create_3d */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t context_init;
    uint64_t cmdbuf;        /* submit_3d: user pointer */
    uint64_t cmdlen;        /* submit_3d */
    char     name[32];      /* ctx_create debug name */
};

#endif
