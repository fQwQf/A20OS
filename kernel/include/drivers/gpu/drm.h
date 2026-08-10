#ifndef _DRIVERS_GPU_DRM_H
#define _DRIVERS_GPU_DRM_H

/*
 * Minimal DRM/KMS device backend (kernel/drivers/gpu/drm.c).
 *
 * Exposes the standard Linux DRM ioctl surface on /dev/dri/card0 for the
 * virtio-gpu / vmsvga drivers, implemented on top of the existing
 * gpu_dev_ops_t (get_info/get_fb/flush).  Supports the dumb-buffer + KMS
 * subset that libdrm-based compositors (weston, kmscube, etc.) need to
 * enumerate modes and present frames:
 *
 *   VERSION, GET_CAP, GEM_CLOSE, PRIME_HANDLE_TO_FD
 *   MODE_GETRESOURCES/GETCRTC/SETCRTC/GETCONNECTOR/GETENCODER/GETPLANE/
 *   GETPLANERESOURCES/GETFB/ADDFB/RMFB/PAGE_FLIP/DPMS/GETPROPERTY/
 *   SETPROPERTY/CREATE_DUMB/MAP_DUMB/DESTROY_DUMB/ATOMIC(test)/GETGAMMA
 *
 * Buffers are anonymous VMO-backed regions (dumb buffers) with a per-fd
 * handle table; MAP_DUMB returns a pseudo-offset that fbdev-style mmap
 * translates through the VMO.
 */

#include "core/types.h"

/* ioctl numbers (Linux ABI, _IOWR('d', nr, size) with size in bits 16-29). */
#define DRM_IOCTL_VERSION          0xc0406400UL
#define DRM_IOCTL_GET_MAGIC        0x80046402UL
#define DRM_IOCTL_AUTH_MAGIC       0x40046411UL
#define DRM_IOCTL_SET_MASTER       0x0000641eUL
#define DRM_IOCTL_DROP_MASTER      0x0000641fUL
#define DRM_IOCTL_GET_CAP          0xc010640cUL
#define DRM_IOCTL_GEM_CLOSE        0x40086409UL
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
#define DRM_IOCTL_MODE_GETFB            0xc01c64adUL
#define DRM_IOCTL_MODE_ADDFB            0xc01c64aeUL
#define DRM_IOCTL_MODE_ADDFB2           0xc06864b8UL
#define DRM_IOCTL_MODE_RMFB             0x400464afUL
#define DRM_IOCTL_MODE_PAGE_FLIP        0xc01864b0UL
#define DRM_IOCTL_MODE_DPMS             0xc00464b1UL
#define DRM_IOCTL_MODE_CREATE_DUMB      0xc02064b2UL
#define DRM_IOCTL_MODE_MAP_DUMB         0xc01064b3UL
#define DRM_IOCTL_MODE_DESTROY_DUMB     0xc00464b4UL
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5UL
#define DRM_IOCTL_MODE_GETPLANE         0xc02064b6UL
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9UL
#define DRM_IOCTL_MODE_ATOMIC           0xc03864bcUL

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
