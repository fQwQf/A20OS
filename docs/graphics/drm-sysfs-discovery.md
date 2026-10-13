# DRM discovery and mount-aware symlinks

Stock libdrm resolves `/sys/dev/char/226:{0,128}`, enumerates the physical
GPU's `drm` directory, and reads its bus identity. The previous directories
and fabricated PCI identity prevented Mesa from discovering the DRM device.

DRM class entries, character-device entries and bus-device entries now link
to one physical tree. PCI names and configuration bytes come from the selected
GPU's enumerated function through `pci_device_location`; MMIO GPUs use the
platform bus and their registered device name. The physical tree exposes both
card0 and renderD128. Generated sysfs children retain their mount so attribute
reads use sysfs operations rather than caching zero-length synthetic files.
This is discovery for the selected display GPU, not full PCI enumeration or
hotplug support.

The generic VFS now uses one mount-aware symlink walker for ordinary lookup
and openat2. Absolute links restart at the process root, retaining remaining
components; RESOLVE_IN_ROOT anchors at the supplied directory and
RESOLVE_BENEATH rejects absolute links. Passing the selected mount explicitly
also preserves bind-mount identity.

Validation (four CPUs, preemption enabled):

- x86_64: `symlink_mount_test`, `drm_sysfs_test`, `cpuinfo_test 4` pass;
  actual GPU path `/sys/devices/pci0000:00/0000:00:02.0`.
- riscv64: same tests plus `resource_ns_test` pass; actual MMIO GPU path
  `/sys/devices/platform/virtio-gpu1`.
- x86_64 `vfs_stress`: PASS, no absent or skipped coverage.
- Old kernel fails the same absolute-link test with ENOENT and DRM lstat test.

Evidence: `build/browser-followup/symlink-mount-{x86,rv,negative}-guest.log`
and `vfs-unified-x86-guest.log`. Reusable gates: `make smoke-symlink-mount`
and `make smoke-drm-sysfs`.
