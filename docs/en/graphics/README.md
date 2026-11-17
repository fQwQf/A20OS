# Index of Documents on Graphics and GPU Subsystem

The A20OS graphics subsystem supports a complete graphics stack that ranges from firmware-based linear frame buffers (EFIFB/SimpleFB) and modern graphics card interfaces via DRM/KMS to 3D hardware acceleration via VirtIO-GPU.

---

## Specifications and Evolutionary Design

| Document | Description |
|---|---|
| [3d-graphics.md](3d-graphics.md) | 3D graphics stack design: Principles of VirtIO-GPU, DRM UAPI, and Mesa 3D acceleration |
| [gpu-3d-roadmap.md](gpu-3d-roadmap.md) | 3D GPU evolution roadmap (VIRTGPU drivers, ABI barriers, and rendering pipelines) |
| [virtgpu-native-allocation.md](virtgpu-native-allocation.md) | VirtIO-GPU native memory allocation and VMO mapping mechanisms |
| [virtgpu-sharing-and-sync.md](virtgpu-sharing-and-sync.md) | Inter-process video memory sharing (dma-buf) and synchronization with homogeneous barriers |
| [drm-sysfs-discovery.md](drm-sysfs-discovery.md) | DRM node detection, sysfs enumeration, and user-space service discovery |
| [firmware-video.md](firmware-video.md) | Boot-time firmware video support (UEFI GOP, Simple Framebuffer) |
| [host-tools.md](host-tools.md) | Host-side graphics toolchain (`virglrenderer` bridging and DRM validation) |
| [real-hardware-gpu.md](real-hardware-gpu.md) | Analysis of the practical limitations of physical hardware GPUs and the feasibility of porting software to them |
