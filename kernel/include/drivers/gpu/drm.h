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
#define DRM_IOCTL_GEM_MMAP         0xc010640bUL
#define DRM_IOCTL_GEM_CREATE       0xc018640cUL
#define DRM_IOCTL_GEM_GET_HANDLE   0xc00c640dUL
#define DRM_IOCTL_GEM_OPEN         0xc0186410UL
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
#define DRM_IOCTL_MODE_RMFB             0x400464afUL
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

/* DRM_IOCTL_MODE_GETFB2: the 32-bit-handle predecessor of ADDFB2.  Mesa's
 * GBM still issues it while probing, so it must not be left as a hole. */
#define DRM_IOCTL_MODE_GETFB2           0xc04864caUL

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
