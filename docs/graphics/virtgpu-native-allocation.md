# Native virtgpu resource allocation

DRM_IOCTL_VIRTGPU_RESOURCE_CREATE now allocates a fresh GEM and host resource,
returning bo_handle and res_handle as outputs. It ignores their input values,
rounds backing storage to pages, and gives size=0 a single page. Failure to
create, attach backing, or copy outputs rolls back both the GEM and host object.
The implementation follows the allocation contract in
[Linux virtgpu_ioctl.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/virtio/virtgpu_ioctl.c).

The GEM store uses dynamically allocated objects in hash buckets instead of a
64-object array. Existing pins and framebuffer references defer destruction;
objects and backing storage are released outside the store mutex. The per-object
256 MiB limit remains. Handle ownership is still device-wide in A20OS; this
change does not claim Linux's per-open GEM handle accounting.

The old test preallocated a private A20 GEM, masking the incompatible resource
creation contract. gpu3d_test now uses only native allocation and checks:

- real host-rendered red and blue pixel readback;
- 96 simultaneous resources, distinct output handles and page-sized backing
  for size=0, followed by close and ENOENT on each retired handle.

Four-CPU preemptive x86_64 and riscv64 guests passed with host virgl. The previous
kernel fails the new request with ENOENT. Logs:
`build/browser-followup/virtgpu-native-{x86,rv,negative}-guest.log`.
These tests establish native allocation and direct command-stream rendering;
Minecraft acceleration requires additional transfer and scanout work.
