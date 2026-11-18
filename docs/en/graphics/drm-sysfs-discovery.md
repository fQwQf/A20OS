# DRM Discovery and Mount-Perceptive Symbolic Links

The stock libdrm library parses `/sys/dev/char/226:{0,128}`, enumerates the `drm` directories of physical GPUs, and reads their bus identities. Previously, the use of fake PCI identities prevented Mesa from detecting DRM devices.

The DRM class entries, character device entries, and bus device entries are now linked to the same physical tree. The PCI names and configuration bytes come from the function enumerated by `pci_device_location` for the selected GPU; MMIO GPUs use the platform bus and their registered device names. The physical tree exposes both card0 and renderD128. The generated sysfs subnodes retain their mount points, so property reads are performed through sysfs operations rather than by caching a synthetic file with a length of zero. This provides discovery support for the selected display GPU, but not full PCI enumeration or hot-plug support.

The general VFS (Virtual File System) now uses the same mount-perceptive symbolic link walker for both regular lookups and openat2: absolute links start from the process root and retain the remaining components; `RESOLVE_IN_ROOT` is anchored to the incoming directory; `RESOLVE_BENEATH` rejects absolute links. Explicitly specifying the mount point also preserves the bind mount identity.

Coverage (for four CPUs, with preemption enabled):

- x86_64: `symlink_mount_test`, `drm_sysfs_test`, `cpuinfo_test 4`; the actual GPU path is `/sys/devices/pci0000:00/0000:00:02.0`.
- riscv64: The above cases plus `resource_ns_test`; the actual MMIO GPU path is `/sys/devices/platform/virtio-gpu1`.
- x86_64 `vfs_stress`: No missing or skipped coverage items.

Reusable build scripts: `make smoke-symlink-mount` and `make smoke-drm-sysfs`.
