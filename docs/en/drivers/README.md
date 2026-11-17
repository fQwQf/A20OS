# A20OS Driver Documentation

How to add a driver for A20OS: From understanding the hardware manual to implementing a driver that is compilable, enumerable, bindable, and cleanable; as well as a list of generic architecture modules and explicit static source code for embedded implementations.

The general implementations are located in `kernel/drivers/`, board-specific adaptations are in `kernel/arch/<arch>/boards/`, common header files are in `kernel/include/drivers/`, and loadable driver module packages are in `kernel/drvmod/examples/`. The full profile does not collect device drivers using class-based directories as a wildcard: `tools/mk/driver-sources.mk` still collects `drivers/core/*.c` and `drivers/bus/*.c`, but generic kernel services and embedded device implementations use explicit source code listings; generic device packages follow the architecture-specific lists in `tools/mk/driver-modules.mk`.

## Classification Navigation

### I. Getting Started and Mechanisms (How to Write Drivers)

| Document | Description |
|---|---|
| [Developing Your First Driver from Scratch](guide/getting-started.md) | Starting with a PCI network card template, going through creation, registration, building, and debugging |
| [Installable Kernel Drivers](guide/kernel-modules.md) | Loadable kernel modules (drvmod), framework export APIs, DriverStore, and migration states |
| [Driver Deployment Profiles](guide/deployment-profiles.md) | Generic architecture module lists, embedded static code bases, Early Runtime DriverStore |
| [Boot and Hardware Discovery](../dev/boot-model.md) | Handover phases, DT/ACPI/static providers, device trees, resources, and dependencies |
| [Core Devices and Driver Models](guide/core-model.md) | `device_t`, `driver_t`, `bus_type_t`, and probe/remove lifecycle |
| [Hotplugging and Plug and Play](guide/hotplug-and-pnp.md) | USB/PCI runtime discovery, ADD/REMOVE lifecycle, and driver writing requirements |
| [Device Class Interfaces](guide/device-classes.md) | Various operations, units, return values, and blocking semantics |
| [Buses and Platforms](guide/bus-and-platform.md) | Platform devices, board-level resources, MMIO/IRQ |
| [PCI and VirtIO](guide/pci-and-virtio.md) | PCI IDs, BARs, ECAM, VirtIO transport |
| [Runtime Contracts](guide/runtime-contracts.md) | MMIO, IRQ, DMA, locking, timeouts |
| [Driver Lock Order Contracts](guide/lock-order.md) | Rules for nesting locks |

### II. Device Class Chapters (By Hardware Category)

| Document | Description |
|---|---|
| [Input Subsystem](classes/input.md) | `/dev/event0` mux, vinput.a20drv module, evdev ioctl interface, dual-resident coordination |
| [Virtio-Console Memory Configuration](classes/virtio-console.md) | Throughput/low-memory presets, queue and buffer configurations, DMA memory budgeting |
| [Display/Framebuffer](classes/display.md) | Framebuffer and GPU drivers |
| [3D Graphics Acceleration Stack](../graphics/3d-graphics.md) | virtio-gpu, virgl 3D: principles, interfaces, implementation, and user-space integration |
| [Audio Subsystem](classes/audio.md) | Audio classes, UAPI, and Intel HDA PCM |
| [USB Subsystem Design](classes/usb-design.md) | USB protocol stack and HID/storage classes |
| [User Space and devfs](classes/userspace-and-devfs.md) | Bridging from classes to `/dev` |

### III. Status and Processes

| Document | Description |
|---|---|
| [Driver Implementation Compliance and Limits](../status/current/drivers-implementation-status.md) | Current implementation matrix: migrated modules, non-migratable boundaries, driver class status |
| [Building, Testing, and Submission](meta/testing-and-submission.md) | Compilation matrix, failure cleanup, and submission evidence |

### IV. Platform and Hybrid Kernel References

| Document | Description |
|---|---|
| [VirtualBox Driver Discovery Chain](../platforms/virtualbox.md) | VirtualBox x86/ARM platform drivers |
| [Dual-Placement Driver Framework](../hybrid-kernel/04-dual-placement.md) | Kernel/user-space dual-resident drivers and ownership arbitration |
| [Hybrid Kernel Mechanisms](../hybrid-kernel/01-mechanisms.md) | Core mechanisms of hybrid kernels |

## Recommended Reading Order

1. [Developing the first driver from scratch](guide/getting-started.md): Start with the PCI network card template and go through the process of creation, registration, building, and debugging.
2. [Installable kernel drivers](guide/kernel-modules.md): Kernel modules, framework APIs, and DriverStore can be loaded; new drivers should preferably be written as modules.
3. [Core models](guide/core-model.md): `device_t`, `driver_t`, `bus_type_t`, and the complete probe/remove templates.
4. Continue reading based on the bus type:
   - [Bus and platform](guide/bus-and-platform.md): Platform devices, board-level resources, and MMIO/IRQ.
   - [PCI and VirtIO](guide/pci-and-virtio.md): PCI IDs, BARs, ECAM, VirtIO transport.
5. [Device classes](guide/device-classes.md): Various operations, units, return values, and blocking semantics; read the relevant chapters before writing input/display/audio drivers.
6. [Runtime contracts](guide/runtime-contracts.md) + [Lock order](guide/lock-order.md): IRQ, DMA, barriers, concurrency.
7. [User interface and devfs](classes/userspace-and-devfs.md): From classes to `/dev`.
8. [Building, testing, and submitting](meta/testing-and-submission.md): Compilation matrices, failure cleanup, and submission of evidence.

## Related documentation

- VirtualBox user manual: For ARM64, see [../platforms/virtualbox-aarch64.md](../platforms/virtualbox-aarch64.md); for x86_64, see [../platforms/virtualbox-x86_64.md](../platforms/virtualbox-x86_64.md).
- Testing guidelines: See [../testing-gates.md](../testing-gates.md).
