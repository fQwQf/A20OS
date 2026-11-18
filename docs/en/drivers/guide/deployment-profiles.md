# Driver Deployment Profiles

A20OS only has two driver deployment profiles: `generic` and `embedded`. These profiles describe how the drivers are deployed, without altering the unified `driver_t`, `device_t`, descriptor, or probe/remove semantics.

## generic

By default, except for the embedded architectures `armv7m`/`ppc64le`, the `DRIVER_DEPLOYMENT=generic` profile is used during construction. Optional hardware must be deployed using `.a20drv` packages listed in the architecture-specific manifest. The driver manager scans these packages and activates them based on the descriptor placement: kernel modules are loaded into the drvmod; user services can only be generated at runtime based on an allowed list of MMIO operations and Native ABI boundaries. Packages marked with `SUPERVISED` are merely recorded and passed to an external supervisor.

The generic profile has two levels of DriverStore:

| Directory | Purpose | Current Status |
|---|---|---|
| `/boot/drivers` | Early DriverStore; selected `.a20drv` packages are linked as read-only blobs into the root ramfs during construction and loaded before the real root disk is mounted | Currently exposed by each architecture’s `early_drivers.c` file; may be missing or empty |
| `/bin/lib/drivers` | Runtime DriverStore; contains standard modules and user services available after the root file system is mounted | Currently uses FAT32 images |

The Early DriverStore is the only mechanism to break the bootstrap loop where a module is both on the root disk and the root disk driver is also a module. The startup strategy can specify the order of early loading, but device identity, resources, and placement can only be specified in the ELF `.a20drv` descriptor; external manifests cannot be used to override these settings.

The startup lifecycle is divided into two phases: `driver_manager_early_init()` is called after `vfs_init()` and before `mount_block_devices()`. During this phase, only `/boot/drivers` is scanned, and only kernel modules are activated. `driver_manager_init()` is called after `proc_init()`, at which point runtime board devices are registered, and `/bin/lib/drivers` is scanned, allowing user services to be generated. This prevents user-mode code from being executed incorrectly before the process subsystems have been initialized.

The generic profile only includes the driver core, buses, and nine explicitly specified service sources from `GENERIC_KERNEL_SERVICE_SRCS` in the image: `loop`, `udisk`, `pty`, `uart`, `framebuffer`, `gpu_core`, `audio_core`, `input_mux`, and `usb_core`. Device packages are listed by architecture using `tools/mk/driver-modules.mk`. The `EMBEDDED_DEVICE_DRIVER_SRCS` cannot be automatically considered part of the generic package set. The Early DriverStore collection also varies by architecture; the Runtime DriverStore only accepts packages that are not part of the Early DriverStore collection, as well as user-service packages specified by the construction rules. Currently, the StarFive/LS2K GMAC does not have generic packages, even though they are listed in the embedded static catalog; therefore, the generic board-level construction does not include them. Any new kernel services, embedded drivers, or generic packages must be added to the respective explicit catalogs and their startup dependencies must be specified.

Board-level devices are registered through the `platform_bus` and carry a `hardware_id`; drivers are matched based on the platform ID. The driver core no longer allows devices without a bus to be preempted by arbitrary, bus-agnostic drivers. Bus-less drivers must provide an explicit `match()` function to be bound (for example, UART serial port services are matched by device name). It is not allowed to copy transport, IRQ, DMA, or class service implementations within a module merely to relocate the source files.

## Embedded
For the "full profile," setting `DRIVER_DEPLOYMENT=embedded` statically links the `EMBEDDED_DEVICE_DRIVER_SRCS` with the kernel services, filters out `driver_manager.c`, does not build the `.a20drv` package, and does not populate or scan the DriverStore. This static configuration is not an union of generic packages: module-only implementations such as RTC, PC speaker, PS/2, NVMe, TPM, HDA, vinput, and vinput-probe will not appear in the embedded image.

The MCU profile is an exception to this construction pattern: For `armv7m`, `PROFILE=mcu` is mandatory, which directly matches the `BOARD_DRIVER_DIR` (currently `kernel/arch/armv7m/boards/stm32f103/drivers/*.c`) and the board-level platform source code, without using the `DRIVER_KERNEL_SRCS` from the full profile. Both `armv7m` and `ppc64le` (which currently lack a drvmod toolchain) are marked as embedded by default, but the actual source code selection for both is determined by their respective MCU and full-profile embedded branches. Other architectures can explicitly choose the embedded profile, but the required capabilities must be verified against the actual build branch.

Module packaging typically involves including shared implementations from `kernel/drivers/` and overriding `DRIVER_REGISTER` to reuse the same protocol source code. This dual deployment is only possible when the packaging and architecture list items are explicitly provided in `tools/mk/driver-modules.mk`. The "embedded" profile is not a second driver model, and it should not result in a deployment API that is only applicable to STM32.

## Build Checks

```sh
make ARCH=riscv64 print-driver-deployment
make ARCH=armv7m print-driver-deployment
make ARCH=x86_64 DRIVER_DEPLOYMENT=embedded kernel-only
```

The profile includes the number of modules as well as the count of `EMBEDDED_DEVICE_DRIVER_SRCS` entries. For MCU profiles, the latter does not represent the actual number of source code files in the `BOARD_DRIVER_DIR`. The build directories for non-MCU architectures are suffixed with `-embedded` to avoid reuse with the default generic profiles. The generic profiles and STM32 profiles maintain the same directory names to ensure compatibility with existing runtime, burning, and testing procedures.

The build rules are organized by responsibility: The top-level `Makefile` retains global variables, the core compilation setup, and the overall entry points. `tools/mk/driver-deployment.mk` defines the profiles, `tools/mk/driver-sources.mk` manages the driver source code, `tools/mk/driver-modules.mk` handles the driver packages and related testing processes, `tools/mk/stm32.mk` is dedicated to STM32-specific settings, and `tools/mk/run-targets.mk` is responsible for runtime, image generation, and debugging mechanisms.
