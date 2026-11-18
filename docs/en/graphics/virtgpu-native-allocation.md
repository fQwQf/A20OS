# Native virtgpu resource allocation

The `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` command now allocates a new GEM (Graphics Memory Entity) and host resources, and returns `bo_handle` and `res_handle` as outputs. It ignores the input values for these two fields, rounds up the backend storage size to the nearest page size, and treats `size=0` as one page. If any step of creating, mounting, or copying the output fails, the GEM and host objects are rolled back. The implementation follows the allocation契约 defined in [Linux virtgpu_ioctl.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/virtio/virtgpu_ioctl.c).

The GEM store has been changed to use dynamically allocated objects in a hash bucket, replacing the previous fixed array of 64 entries. Existing pin and framebuffer references are not destroyed immediately; objects and backend storage are released outside of the store’s mutexes. The upper limit of 256 MiB per object remains unchanged. The ownership of handles remains at the device level in A20OS; this change does not affect Linux’s per-open GEM handle accounting.

Old tests would pre-allocate a private A20 GEM, which masked issues with incompatible resource creation protocols. `gpu3d_test` now only uses native allocation and checks the following:
- That the red and blue pixels actually rendered by the host are read back;
- That 96 concurrent resources with unique output handles are created, with the backend size set to `size=0` (i.e., one page size), and that these handles are closed one by one, resulting in an ENOENT (Error No Entropy) error for the closed handles.

This change establishes native allocation and direct rendering via the command stream; additional transmission and scanout operations are required for Minecraft acceleration.
