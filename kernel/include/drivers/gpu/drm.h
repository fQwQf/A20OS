#ifndef _DRIVERS_GPU_DRM_H
#define _DRIVERS_GPU_DRM_H

/*
 * Minimal DRM/KMS device backend (kernel/drivers/gpu/drm.c).
 *
 * Exposes the standard Linux DRM ioctl surface on /dev/dri/card0 for the
 * virtio-gpu / vmsvga drivers, implemented on top of the existing
 * gpu_dev_ops_t (get_info/get_fb/flush).  Supports the buffer-object + KMS
 * subset that libdrm-based compositors (wlroots/labwc, weston, kmscube) and
 * Mesa's software/GBM backends need:
 *
 *   VERSION, GET_CAP, SET_CLIENT_CAP
 *   GEM_CREATE, GEM_OPEN, GEM_MMAP, GEM_CLOSE, GEM_GET_HANDLE
 *   PRIME_HANDLE_TO_FD, PRIME_FD_TO_HANDLE
 *   MODE_GETRESOURCES/GETCRTC/SETCRTC/GETCONNECTOR/GETENCODER/GETPLANE/
 *   GETPLANERESOURCES/GETFB/GETFB2/ADDFB/ADDFB2/RMFB/PAGE_FLIP/DPMS/
 *   GETPROPERTY/SETPROPERTY/GETPROPBLOB/CREATE_DUMB/MAP_DUMB/DESTROY_DUMB/
 *   ATOMIC(test)/GETGAMMA
 *
 * All buffers are GEM objects (drm_gem_t) backed by an anonymous VMO, and
 * dumb buffers are GEM objects with a linear layout -- the same shape Linux
 * uses, so there is only one buffer abstraction to reason about.
 *
 * mmap offsets encode the handle: GEM_MMAP and MAP_DUMB both return
 * `handle << PAGE_SHIFT`, and mmap() translates that back through the VMO.
 * That is what lets one mmap path serve dumb buffers and GEM objects alike.
 */

#include "core/types.h"

/* ioctl numbers (Linux ABI, _IOWR('d', nr, size) with size in bits 16-29). */
#define DRM_IOCTL_VERSION          0xc0406400UL
#define DRM_IOCTL_GET_MAGIC        0x80046402UL
#define DRM_IOCTL_AUTH_MAGIC       0x40046411UL
#define DRM_IOCTL_SET_MASTER       0x0000641eUL
#define DRM_IOCTL_DROP_MASTER      0x0000641fUL
#define DRM_IOCTL_GET_CAP          0xc010640cUL
#define DRM_IOCTL_SET_CLIENT_CAP   0x4010640dUL
#define DRM_IOCTL_GEM_CLOSE        0x40086409UL
#define DRM_IOCTL_GEM_FLINK        0xc008640aUL
#define DRM_IOCTL_GEM_OPEN         0xc010640bUL
#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0xc00c642dUL
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xc00c642eUL
#define DRM_IOCTL_MODE_GETRESOURCES    0xc04064a0UL
#define DRM_IOCTL_MODE_GETCRTC          0xc06864a1UL
#define DRM_IOCTL_MODE_SETCRTC          0xc06864a2UL
#define DRM_IOCTL_MODE_GETGAMMA         0xc02064a4UL
#define DRM_IOCTL_MODE_GETENCODER       0xc01464a6UL
#define DRM_IOCTL_MODE_GETCONNECTOR     0xc05064a7UL
#define DRM_IOCTL_MODE_GETPROPERTY      0xc04064aaUL
#define DRM_IOCTL_MODE_SETPROPERTY      0xc01064abUL
#define DRM_IOCTL_MODE_GETPROPBLOB      0xc01064acUL
#define DRM_IOCTL_MODE_GETFB            0xc01c64adUL
#define DRM_IOCTL_MODE_ADDFB            0xc01c64aeUL
#define DRM_IOCTL_MODE_ADDFB2           0xc06864b8UL
#define DRM_IOCTL_MODE_RMFB             0xc00464afUL
#define DRM_IOCTL_MODE_PAGE_FLIP        0xc01864b0UL
#define DRM_IOCTL_MODE_DPMS             0xc00464b1UL
#define DRM_IOCTL_MODE_CURSOR           0xc01c64a3UL
#define DRM_IOCTL_MODE_CURSOR2          0xc02464bbUL
#define DRM_IOCTL_MODE_CREATE_DUMB      0xc02064b2UL
#define DRM_IOCTL_MODE_MAP_DUMB         0xc01064b3UL
#define DRM_IOCTL_MODE_DESTROY_DUMB     0xc00464b4UL
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5UL
#define DRM_IOCTL_MODE_GETPLANE         0xc02064b6UL
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9UL
#define DRM_IOCTL_MODE_ATOMIC           0xc03864bcUL

/* DRM_IOCTL_MODE_GETFB2: shares drm_mode_fb_cmd2 with ADDFB2 but is command
 * 0xCE, not 0xB8.  Mesa's GBM issues it while probing, so it must not be left
 * as a hole. */
#define DRM_IOCTL_MODE_GETFB2           0xc06864ceUL

/* ---- A20-private buffer ioctls -------------------------------------------
 *
 * The Linux UAPI has no GEM_CREATE, GEM_MMAP or GEM_GET_HANDLE: userspace
 * buffer creation goes through MODE_CREATE_DUMB (or a driver-specific ioctl),
 * and naming an existing handle for cross-process import is GEM_FLINK above.
 * The three below therefore have no upstream number to match, and they used to
 * sit on plausible-looking hex values that were not any Linux ioctl at all --
 * one of which (0xc010640b) is in fact DRM_IOCTL_GEM_OPEN, so a guest calling
 * the real GEM_OPEN was silently served by the mmap handler.
 *
 * They are kept because they are the only way to hand a sized, non-scanned-out
 * buffer to the 3D path, but they now live in the private range where they
 * cannot be confused with, or shadow, a Linux ioctl.  tools/check-drm-abi.sh
 * checks everything outside this block against the installed UAPI headers.
 */
#define A20_GPU_IOCTL_GEM_CREATE       0x00004710UL
#define A20_GPU_IOCTL_GEM_MMAP         0x00004711UL

/* ---- virtio-gpu 3D UAPI (include/uapi/drm/virtgpu_drm.h) ----------------
 *
 * This is the interface Mesa's virtio_gpu_dri.so actually speaks.  It is NOT
 * the legacy DRM_IOCTL_VIRGL_* family (command letter 'A'); that one is unused
 * by modern Mesa.  Commands sit at DRM_COMMAND_BASE + n, type letter 'd'.
 * Numbers below were derived from the kernel uapi header and checked against
 * the _IOWR encoding; see docs/graphics/gpu-3d-roadmap.md section 1.
 */
#define DRM_VIRTGPU_MAP                  0x40 + 0x01
#define DRM_VIRTGPU_EXECBUFFER           0x40 + 0x02
#define DRM_VIRTGPU_GETPARAM             0x40 + 0x03
#define DRM_VIRTGPU_RESOURCE_CREATE      0x40 + 0x04
#define DRM_VIRTGPU_RESOURCE_INFO        0x40 + 0x05
#define DRM_VIRTGPU_TRANSFER_FROM_HOST   0x40 + 0x06
#define DRM_VIRTGPU_TRANSFER_TO_HOST     0x40 + 0x07
#define DRM_VIRTGPU_WAIT                 0x40 + 0x08
#define DRM_VIRTGPU_GET_CAPS             0x40 + 0x09
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x40 + 0x0a
#define DRM_VIRTGPU_CONTEXT_INIT         0x40 + 0x0b

#define DRM_IOCTL_VIRTGPU_MAP            0xc0106441UL
#define DRM_IOCTL_VIRTGPU_EXECBUFFER     0xc0406442UL
#define DRM_IOCTL_VIRTGPU_GETPARAM       0xc0106443UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE 0xc0386444UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO  0xc0106445UL
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST 0xc02c6446UL
#define DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST   0xc02c6447UL
#define DRM_IOCTL_VIRTGPU_WAIT           0xc0086448UL
#define DRM_IOCTL_VIRTGPU_GET_CAPS       0xc0186449UL
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB 0xc030644aUL
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT   0xc010644bUL

/* VIRTGPU_PARAM_* queried through DRM_IOCTL_VIRTGPU_GETPARAM.  Mesa asks for
 * these before anything else and abandons the device if the answers are
 * wrong, so they are the cheapest high-value thing to get right. */
#define VIRTGPU_PARAM_3D_FEATURES            1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX       2
#define VIRTGPU_PARAM_RESOURCE_BLOB          3
#define VIRTGPU_PARAM_HOST_VISIBLE           4
#define VIRTGPU_PARAM_CROSS_DEVICE           5
#define VIRTGPU_PARAM_CONTEXT_INIT           6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs   7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME    8

#define VIRTGPU_EXECBUF_FENCE_FD_IN     0x01
#define VIRTGPU_EXECBUF_FENCE_FD_OUT    0x02
#define VIRTGPU_EXECBUF_RING_IDX        0x04
#define VIRTGPU_EXECBUF_FLAGS \
    (VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT | \
     VIRTGPU_EXECBUF_RING_IDX | 0)

#define VIRTGPU_WAIT_NOWAIT 1

/* Keys in the ctx_set_params array that DRM_IOCTL_VIRTGPU_CONTEXT_INIT
 * carries.  Mesa uses CAPSET_ID/CAPSET_VERSION to choose between the capset 1
 * and capset 2 command protocols, so discarding these is what makes a client
 * fall back to the one protocol it guessed. */
#define VIRGLPARAM_CTX_ID         0
#define VIRGLPARAM_CTX_RESET      1
#define VIRGLPARAM_CAPSET_ID      2
#define VIRGLPARAM_CAPSET_VERSION 3

/* Capability ids for DRM_IOCTL_GET_CAP.  These are libdrm's legacy
 * DRM_CAP_* numbering, which is *not* the same order as the newer
 * DRM_CAP_* in include/uapi/drm/drm.h -- verified against
 * libdrm/drm.h and cross-checked against drm_get_cap() in drm.c. */
#define DRM_CAP_DUMB_BUFFER           0x1
#define DRM_CAP_VBLANK_HIGH_CRTC      0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH  0x3
#define DRM_CAP_DUMB_PREFER_SHADOW    0x4
#define DRM_CAP_PRIME                 0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC   0x6
#define DRM_CAP_ASYNC_PAGE_FLIP       0x7
#define DRM_CAP_CURSOR_WIDTH          0x8
#define DRM_CAP_CURSOR_HEIGHT         0x9
#define DRM_CAP_ADDFB2_MODIFIERS      0x10
#define DRM_CAP_PAGE_FLIP_TARGET      0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT  0x12
#define DRM_CAP_SYNCOBJ               0x13

#define DRM_PRIME_CAP_IMPORT 0x2
#define DRM_PRIME_CAP_EXPORT 0x1

/* BO usage flags (include/uapi/drm/drm_mode.h), as passed by GBM. */
#define DRM_BO_USE_SHAREABLE   (1 << 1)
#define DRM_BO_USE_LINEAR      (1 << 3)
#define DRM_BO_USE_RENDERING   (1 << 5)

#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN   32

/* Create/open the DRM device node backend.  Returns a global VFS fd. */
int drm_create_file(void);

/* Create a DRM vfile (used by the /dev/dri/card0 devfs open path). */
struct vfile *drm_create_vfile(void);

/* Wire the DRM backend to the GPU driver at device probe time. */
void drm_device_bind(void);

/* True if vf is an open /dev/dri/card0 file. */
int drm_is_drm_vfile(struct vfile *vf);

/* Map a DRM dumb buffer (offset == buffer handle) into the caller. */
int64_t drm_linux_mmap(struct vfile *vf, uint64_t addr, size_t len, int prot,
                       int flags, uint64_t off);

#endif /* _DRIVERS_GPU_DRM_H */
