# Build and Run Guide

This guide covers toolchain prerequisites, build/run commands and common configuration. See [module status](../status/current/module-status.md) for implementation and validation, and the [1.0 plan](../releases/v1.0.md) for implementation choices and release conditions. The [1.0 platform scope](../releases/v1.0.md#2-平台范围) excludes LoongArch32/PPC64LE. Active Make, instance, CI and test matrices no longer include these historical targets, and normal builds explicitly reject them. Historical sources and instances remain for reference. To reproduce them, check out `v0.18` in a clean checkout and follow the platform documents; historical build records are not current support commitments.

Run the common A20OS build and execution commands from the repository root. See [architecture.md](../architecture.md) for detailed design.

> Build/run/smoke configurations can also be declared as TOML instances under `instances/`, driven by `tools/a20 run|build|debug|test <实例>`. Per-architecture `run-*`/`debug-*`/`smoke-*` make targets are thin wrappers. See [instances.md](instances.md) for fields, component registries and validation checks.

> See [packaging/overview.md](../packaging/overview.md) for apk packaging, manifest-based image assembly and publishing; see [packaging/ci.md](../packaging/ci.md) for CI containers and reproducible build environments.

## First run: Linux + RISC-V 64 {#首次运行linux--risc-v-64}

Complete this serial-console path before trying a desktop or other architectures. No board, host GUI or hardware virtualization is required; QEMU uses software emulation. The native toolchain baseline here is x86_64 Debian 13, GCC 14.2, QEMU 10.0.13, Python 3.13 and Rust 1.96.0. Installation instructions also include Ubuntu 24.04, whose distribution toolchain combination must be assessed using actual build and smoke results.

Budget 8 GiB of host RAM and 20 GiB of free disk space; use `-j4` initially to control compilation concurrency. Container images also consume Docker storage. Actual usage depends on configuration; these are preparation budgets. The serial guest defaults to 1 GiB of memory. Pre-launch checks inspect free memory, disk space and other instances, and may wait when resources are insufficient; see troubleshooting below.

### 1. Get the complete source

Install Git first (Debian/Ubuntu: `sudo apt-get update && sudo apt-get install -y git`), then run:

```bash
git clone --recurse-submodules https://github.com/fQwQf/A20OS.git
cd A20OS
```

For an existing ordinary clone, run `git submodule update --init --recursive`. fastfetch is a submodule pinned to a commit; an ordinary clone or GitHub source ZIP does not include its contents. The basic user-space build currently skips missing fastfetch, so a completed build does not guarantee the complete program set. The environment check below explicitly reports it missing. Initialize submodules again after updating the code.

### 2. Install single-architecture dependencies

This list prepares only the RISC-V64 serial development image; it does not require LoongArch, ARM or desktop dependencies:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential git curl ca-certificates xz-utils file python3 \
    dosfstools e2fsprogs mtools util-linux \
    gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
    gcc-riscv64-linux-gnu libc6-dev-riscv64-cross

# Debian 13：
sudo apt-get install -y qemu-system-riscv
# Ubuntu 24.04 用下面这一条替代上一条：
# sudo apt-get install -y qemu-system-misc
```

Python must be 3.11 or newer, with only its standard library required. This path does not require pip, Conda, CMake or GRUB. `riscv64-unknown-elf-gcc` compiles the kernel and static user space; Linux cross toolchains and headers serve builds including fastfetch. Install cross libc headers explicitly so `--no-install-recommends` does not leave a compiler without headers.

Rust is managed through rustup. If rustup is not installed, run:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
```

Existing rustup users should also run:

```bash
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 \
    riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf
rustc --version
python3 tools/check_quickstart.py
```

The repository's `rust-toolchain.toml` selects 1.96.0 without changing the default outside the repository. Put `$HOME/.cargo/bin` near the start of PATH. An existing `RUSTUP_TOOLCHAIN` environment variable or rustup directory override takes precedence; remove obsolete overrides. Distribution `rustc` does not read rustup's selection. The RISC-V64 first-run path needs only the two prebuilt core targets above, not `rust-src`. The environment check compiles temporary files to inspect tools, target cores, cross headers and submodules. It neither installs dependencies nor establishes that the entire kernel builds.

### 3. Build, boot and exit

Run from the repository root, using ordinary user privileges for compilation and QEMU:

```bash
tools/a20 run qemu-riscv64 -- -j4
```

This builds the kernel, user space and disk image, then starts serial QEMU. The first build is slower than later incremental builds. Do not run multiple builds in one checkout or modify an image in use by a guest. At the shell prompt, run:

```sh
/bin/echo hello
poweroff
```

Expect a standalone `hello` line, followed by `System is going down for power-off NOW`. QEMU should exit back to the host shell. Use `poweroff` for normal shutdown. If the guest stops responding, press `Ctrl-a` and then `x` to force serial QEMU to exit; this does not count as normal-shutdown acceptance.

The complete flow can also be checked automatically:

```bash
make -j4 smoke-quickstart
```

This check uses the same both-ABI development image as the first-run path. It validates shell built-ins, the external `/bin/echo` program, the shutdown marker and QEMU exit code 0. Timeouts and abnormal exits fail. Results are in `.kernel-build/smoke/smoke-quickstart.log`. `make smoke-riscv64` only checks kernel-only bring-up and cannot replace user-space acceptance.

Bare `make` / `make all` performs the RISC-V64 + LoongArch64 release build, not the first-run path. Use `tools/a20 build qemu-riscv64 -- -j4` to build without launching.

### Optional: use a build container

With Docker and the complete source, build a single-architecture environment from the repository Dockerfile to avoid installing host toolchains. The Docker daemon must be available, with access to Debian repositories and Rust downloads. This does not depend on a prepublished GHCR tag; old local images do not update automatically when source changes.

```bash
tools/ci/build-image.sh riscv
docker run --rm -it -v "$PWD:/src" -w /src a20os-buildenv:riscv bash
# 以下在容器内执行：
python3 tools/check_quickstart.py
tools/a20 run qemu-riscv64 -- -j4
```

The script builds the image and checks its toolchain. The Dockerfile pins Rust, but Debian packages follow repository updates; this is not an environment frozen by package hashes. By default the container writes mounted directories as root, so artifacts may be root-owned. When returning to host builds, exit the container and, if needed, run `sudo chown -R "$(id -u):$(id -g)" .kernel-build user/build build`. See [CI/CD](../packaging/ci.md) for images, caches and CI details.

### Common blockers

| Symptom | Resolution |
| --- | --- |
| `rustup: command not found` | Install rustup and run `. "$HOME/.cargo/env"`; apt rustc/cargo alone is insufficient |
| `can't find crate for core` | Install the two targets for **1.96.0** as above; inspect overrides and PATH with `rustup show active-toolchain` |
| `tomllib` / `assert_never` import failure | Python is too old; use Python 3.11+ |
| Missing `bits/libc-header-start.h` or Linux UAPI headers | Install the architecture's `libc6-dev-*-cross`, not just cross gcc |
| Environment check reports missing fastfetch | Run `git submodule update --init --recursive` and check GitHub connectivity |
| Pre-launch resource shortage and waiting | Inspect budgets with `tools/a20 resources`; free RAM/disk or stop unused instances. Bound waiting with `tools/a20 run qemu-riscv64 --wait-timeout 60 -- -j4` |
| Image in use or instance busy | Exit this checkout's existing QEMU instance; retry after it stops |
| Environment check passes but C/Rust compilation fails | Save the full command, `git rev-parse HEAD`, compiler version and first error. This is a build problem; do not disable `-Werror` to conceal it |
| GUI startup fails | The serial quick path requires no display service; desktop use additionally needs display, graphics and audio backends. Follow the [distribution documentation](../distro/README.md) |
| Alpine world download returns 403 or times out | Set `ALPINE_MIRROR_ROOT=https://dl-cdn.alpinelinux.org/alpine` for world builds; the serial quick path does not download an Alpine world |

## Other platforms and current limitations

The native Linux quick path is this page's acceptance focus. Windows users may try a Linux VM or WSL2; macOS users may try a Linux container. These host combinations have not received end-to-end acceptance here, and native command portability is not promised. A booting desktop, successful kernel bring-up, a working shell and usable physical hardware are distinct levels. Consult the [delivery baseline](../status/current/delivery-baseline.md) and [gap tracker](../status/gaps/README.md). STM32 currently has a Rust-target parsing blocker for `ARCH=armv7m`; installing ARM gcc does not resolve it. Its commands are maintenance entry points only; see [driver gaps](../status/gaps/drivers.md).

To add architectures after installing common tools and rustup, install the corresponding dependencies:

| Architecture | Debian 13 toolchain / QEMU packages | 1.96.0 Rust target |
| --- | --- | --- |
| x86_64 | `gcc-x86-64-linux-gnu qemu-system-x86` | `x86_64-unknown-none` |
| AArch64 | `gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-system-arm` | `aarch64-unknown-none`; also install `rust-src` |
| LoongArch64 | `gcc-loongarch64-linux-gnu libc6-dev-loong64-cross qemu-system-misc` | `loongarch64-unknown-none` |
| ARM32 | `gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-system-arm` | `armv7a-none-eabihf` |
| RISC-V32 | Shares toolchain and QEMU with RISC-V64 | Custom target; also install `rust-src` |
| ARMv7-M | `gcc-arm-none-eabi binutils-arm-none-eabi qemu-system-arm openocd` | `thumbv7m-none-eabi`; still subject to the blocker above |

For the selected architecture, run `rustup target add --toolchain 1.96.0 <表中的目标>`. The AArch64 kernel builds its own core for `aarch64-unknown-none-softfloat`, while user space uses `aarch64-unknown-none`. RISC-V32 uses the custom JSON target `tools/rust/targets/riscv32imafdc-a20.json`. These two kernel builds require:

```bash
rustup component add --toolchain 1.96.0 rust-src
```

The build entry point sets `RUSTC_BOOTSTRAP=1` and builds core through `-Z build-std=core`. The first Cargo run may download dependencies. A separate nightly installation is unnecessary. See [target runtimes](tooling-internals.md#target-runtime) for technical background.

Notes:

- LoongArch64: Ubuntu 24.04 apt repositories lack `gcc-loongarch64-linux-gnu`; install the official Loongson cross toolchain separately.
- LoongArch32 (LA32R) and PPC64LE have left the current build-environment inventory. Historical material does not establish current support; use a clean `v0.18` checkout for reproduction.
- Cross gcc lists `libc6-dev-*-cross` only as **Recommends**: `--no-install-recommends` can install a compiler with incomplete headers. Some kernel files include libc headers, such as `kernel/ipc/keyring.c` including `<limits.h>`, so the corresponding `libc6-dev-*-cross` packages are listed explicitly. They also bring `linux-libc-dev-*-cross` UAPI headers.
- arm32: `libc6-dev-armhf-cross` provides libc/UAPI headers for kernel and user space. If you only unpack a cross toolchain into a temporary directory, point `FF_LINUX_UAPI_ROOT` at the root containing `<triple>/include`, for example `make FF_LINUX_UAPI_ROOT=/tmp/arm-root/usr check-build-matrix`. This variable only affects **user-space** UAPI header lookup (`user/Makefile`), not kernel headers. ARMv7-M uses the separate STM32 target and a different toolchain.
- See [time ABI](tooling-internals.md#arm32-time-abi) for ARM32 timer regression-fixture maintenance.
- Python: the Makefile defaults to `python3` (Python 3.11 or newer, standard library only). Conda is unnecessary and is not selected automatically when installed. Override the interpreter explicitly with `make PYTHON=/path/to/python3 ...`.
- `grub-pc-bin` / `grub-efi-amd64-bin` / `xorriso`: only needed for x86_64 thin-client deployment. When `tools/a20 package x86_64-pc` creates a GRUB rescue ISO, `grub-mkrescue` calls `xorriso`; missing it fails packaging with `grub-mkrescue: 未找到 xorriso`. riscv64/aarch64 boards and QEMU VMs do not require these packages.

## Common build and run commands

| Command | Purpose | Use case |
|------|------|----------|
| `make ARCH=riscv64 BOARD=qemu-virt-riscv64 run` | Build kernel, user space and file system image, then boot QEMU | Normal development |
| `make ARCH=riscv64 run` | Same, with `BOARD` defaulting to `qemu-virt-riscv64` | Quick launch |
| `make run-riscv64` | Equivalent to `make ARCH=riscv64 run` | Short target name |
| `make run-loongarch64` / `make run-arm64` / `make run-x86_64` | Boot QEMU for the selected architecture | Cross-architecture tests |
| `make ARCH=riscv64 BRINGUP=1 kernel-only` | Build only the kernel, without a file system image | Kernel-only changes |
| `make ARCH=riscv64 BRINGUP=1 run` | Build only the kernel and boot QEMU | Kernel bring-up |
| `make ARCH=riscv64 NOMMU=1 run` | Run in NOMMU mode | NOMMU testing |
| `make debug-riscv64` | Build with `-O0 -g -DDEBUG` and start QEMU waiting for GDB | Source debugging |
| `make all` | Build dual-architecture release artifacts: `kernel-rv`, `kernel-la`, `disk.img`, `disk-la.img` | Release builds |
| `make dev-build` | Produce kernel, FAT32 and ext4 images | Complete user space |
| `make kernel-only` | Produce only the kernel | Fast compilation checks |
| `make stm32f103-bringup` | Produce STM32F103 firmware for 64 KiB Flash / 20 KiB SRAM | Initial MCU work |
| `make stm32f103-xuanwu` | Produce Xuanwu-board firmware for 512 KiB Flash / 64 KiB SRAM | Puzhong Xuanwu board |
| `make run-stm32f103-qemu` | Build firmware for 128 KiB Flash / 8 KiB SRAM and start `stm32vldiscovery` | Requires `qemu-system-arm` |
| `make flash-stm32f103-xuanwu` | Build Xuanwu firmware and flash with OpenOCD | Requires OpenOCD, CMSIS-DAP and hardware |
| `tools/a20 test qemu-riscv32-mcu` | Build and run an RV32 M-mode kernel plus CMSIS experiments 4.1–4.5 in QEMU | Requires `qemu-system-riscv` and an RV32IMC toolchain |
| `tools/a20 flash hi3861` | Build and flash Hi3861 kernel A; `prepare` fetches pinned loaderboot | Requires an RV32IMC toolchain, CH340 serial port and board reset |
| `make -C user ARCH=riscv64` | Build RISC-V user space separately | User-program changes only |
| `make check-user-build` | Compile the host's default hosted-user-space set (six architectures on Linux, RISC-V64 on macOS) | Routine pre-submission check |
| `make check-build-matrix-all` | Explicitly compile kernel bring-up and user space for six hosted architectures | Complete active build matrix |

STM32 firmware, QEMU and flashing targets share `BUILD_DIR` naming and remain subject to the Rust-target blocker above. QEMU execution and physical-board flashing additionally require `qemu-system-arm`, OpenOCD and actual debug hardware respectively. See the [STM32F103 port](../platforms/stm32f103-port.md) for exact artifact paths.

## Declarative build configuration (instances/ and components/trim.toml)

Two declarative entry points complement the Makefile and should be preferred to assembling make variables manually:

- **Instance manifests**, `instances/*.toml`: each file describes a complete buildable/runnable/testable system. `tools/a20 build|run|flash|... <实例>` validates it and derives make variables. See [instances.md](instances.md).
- **Trimming registry**, `components/trim.toml`: kernel-trimming **policy** data, including profile source lists such as `[profile.mcu]`, architecture matrices for optional capabilities (nommu/swap/xlator/ramfs-user/verified SMP platforms/PCIe MMIO) and fixed per-architecture macros. Makefile reads generated `components/trim.mk`; `NOMMU_SUPPORTED_ARCHES`, `SWAP_SUPPORTED_ARCHES` and MCU source lists originate here. `tools/a20` reads the same TOML to validate instances. After editing, run `make regen-trim-fragment`. `make check-trim-registry` rejects stale fragments, missing entries and unused configuration.

Use `tools/a20 trim <实例>` to see an instance's complete plan: profile, capability switches and reasons, and memory budget.

## Release and debug modes

- The default is `OPT=-O3` for release builds.
- `make debug-<arch>` forces `OPT="-O0 -g -DDEBUG"` and starts QEMU's GDB server.

## Common variables

- `ARCH`: target architecture, such as `riscv64`, `aarch64`, `x86_64`, `loongarch64`, `arm32`, `riscv32` or `armv7m`. LoongArch32/PPC64LE are historical targets rejected by active builds; use `v0.18` for reproduction.
- `BOARD`: defaults to `qemu-virt-<ARCH>`; STM32 uses `stm32f103`.
- `BRINGUP`: `1` builds only the kernel; `0` builds complete user space.
- `ABI`: `linux` / `native` / `both`, default `both`.
- `NR_CPUS`: default `1`. Only the corresponding QEMU virt boards for `riscv64`, `aarch64`, `loongarch64` and `x86_64` are on the verified SMP allowlist, defined in `components/trim.toml` by its `smp-verified-qemu` matrix.
- `CONFIG_KERNEL_PREEMPT`: default `1`, enabling kernel preemption on hosted architectures; see `docs/zh/kernel/process-scheduler.md` §4.1. The MCU profile (armv7m) disables it. Set `0` for a cooperative kernel. This switch participates in `BUILD_VARIANT` (the `-preempt` directory suffix), so changing it does not reuse stale objects from the old configuration.
- `NOMMU`: `1` enables NOMMU. Build support includes `riscv64`, `riscv32`, `aarch64`, `arm32` and `armv7m`, as defined in `components/trim.toml` by its `nommu` matrix. The hosted MMU/NOMMU runtime matrix covers only the first four; ARMv7-M uses its own MCU entry point.
- `DRIVER_DEPLOYMENT`: hosted development usually defaults to `generic`, packaging discoverable drivers as `.a20drv`; `embedded` statically links the complete driver set. ARMv7-M and release builds use embedded.
- `QEMU_GUI_AUDIO_DRIVER`: host audio backend for graphical RISC-V/x86_64/LoongArch64 QEMU. Defaults to `pa` on Linux and `coreaudio` on macOS; alternatives include `pipewire`, `alsa`, `sdl` and `none`.
- `QEMU_GUI_AUDIO_DEVICE`: PCM controller, default `hda`; set `virtio` for QEMU virtio-sound.

> The desktop now uses Alpine xfce world: `make image-world PKG_WORLD=xfce` assembles an image and `make run-world-gui PKG_WORLD=xfce` launches it graphically. The former source-built Weston/XFCE path using `user/wayland` + `user/external/gui`, and the LVGL native desktop, are retired and archived under `archive/legacy-desktop`.

## QEMU audio

`make run-world-gui PKG_WORLD=xfce` attaches a standard Intel HDA controller and duplex codec by default. Switch to the second PCM backend with the command below; RISC-V uses VirtIO-MMIO, while x86_64 and LoongArch64 use modern VirtIO PCI:

```bash
make run-world-gui PKG_WORLD=xfce QEMU_GUI_AUDIO_DEVICE=virtio
```

After boot, verify PCM output in the terminal:

```bash
audioplay --tone 440
audioplay music.wav
```

WAV input must be 48 kHz, stereo, S16_LE PCM. Use `audioplay --raw file.pcm` for raw PCM. The player discovers PCM devices through `GET_CAPS`, without assuming a particular driver or dynamic number. PCM clients can wait for submitted audio with `A20_AUDIO_IOCTL_DRAIN`; closing a device also drains it. If the host uses PipeWire without PulseAudio compatibility, run `make run-world-gui PKG_WORLD=xfce QEMU_GUI_AUDIO_DRIVER=pipewire`.

## Notes

- `BRINGUP=1` does not generate file system images; `BRINGUP=0` triggers user-space and disk builds.
- Builds sharing a user-space output directory are serialized. Different optimization settings or profiles can be requested concurrently; waiting builds proceed when the directory is available. See the [concurrent-build contract](tooling-internals.md#concurrent-builds) when maintaining this wiring.
- `make clean` removes the checkout's configured `.kernel-build` contents. If that path is a symlink to dedicated external build storage, it preserves the link and clears architecture build directories in its target. Before using external storage, verify that it is dedicated to A20OS and create a marker with `printf 'a20-build-root-v1\n' > /path/to/cache/.a20-build-root`. Cleanup validates the marker, rejects links to the repository or its parents, and rejects unexpected architecture-directory names to avoid deleting unrelated files.
- `NR_CPUS=1` is the default. RISC-V 64, AArch64, LoongArch64 and x86_64 QEMU virt platforms have verified SMP and accept `NR_CPUS>1` directly. Other architectures or boards are rejected unless `ALLOW_UNVERIFIED_SMP=1` is explicitly set.
- The `make all` release build targets RISC-V64 and LoongArch64 with `PROFILE=benchmark NR_CPUS=8 DRIVER_DEPLOYMENT=embedded EXTERNAL_ROOT=1`, producing `kernel-rv`, `kernel-la`, `disk.img` and `disk-la.img` at the root. Optimization still defaults to `OPT=-O3`; use `OPT` to change it.
- `ARCH=armv7m` requires `arm-none-eabi-gcc` or `clang` + `llvm-objcopy`.
- Graphical QEMU targets require host display support; use ordinary `run-*` targets without a GUI.
- Avoid writing QEMU commands by copying parameters outside the Makefile; this can omit essential options such as `-bios default`.
