# A20OS Documentation

Welcome to the A20OS design and development documentation. See the [1.0 plan](releases/v1.0.md) and [acceptance plan](releases/v1.0-validation.md) for system 1.0 scope and technical decisions.

- **First run and quick start**: begin with the [build and run guide](dev/build.md#首次运行linux--risc-v-64). Prepare the environment, then start an interactive RISC-V 64 system with `tools/a20 run qemu-riscv64 -- -j4`. Enter `poweroff` at the shell to shut down normally.
- **System architecture**: read [architecture.md](architecture.md) for the hybrid kernel, dual ABI mechanisms and module organization; see the [boot model](dev/boot-model.md) for boot and hardware discovery.
- **Documentation website**: the online documentation uses MkDocs Material. Preview it locally with `mkdocs serve` or build it with `bash site/build.sh`.

---

This collection covers A20OS design and development. A20OS is a **hybrid kernel**: performance-critical paths (scheduling, MM, VFS and the page cache) remain in kernel space. Drivers can be deployed as kernel `.a20drv` packages under the generic profile or linked statically under the embedded profile. The Native ABI also provides user-space driver services and file system hosting (uxfs/ufsd). The Linux ABI supports the existing musl ecosystem, while the Native ABI exposes capabilities, handles and events. Registered entries are defined in each `syscall_table.def`; their count does not establish semantic completeness. See the [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md) for architectural background.

## Document scope and authority

- Current factual references: this page, [architecture.md](architecture.md), [dev/build.md](dev/build.md), [dev/instances.md](dev/instances.md), [kernel/process-scheduler.md](kernel/process-scheduler.md) and [testing-gates.md](testing-gates.md) target current source interfaces. They do not independently establish runtime results.
- Directory responsibilities: the root contains entry documents only (this page, contributing, architecture, validation checks, acknowledgments and license notices). `dev/` covers building, running, debugging and external dependencies. `kernel/` contains core kernel references (scheduling, memory management, Linux ABI details and architecture features). Other directories are organized by subsystem (`drivers/`, `fs/`, `net/`, `graphics/`, `native-abi/`, `cluster/`, `hypervisor/`, `platforms/`, and others).
- Design and planning documents: `hybrid-kernel/`, `hypervisor/`, `roadmap/` and pages explicitly labeled plan/design may describe intended capabilities. Content without corresponding source and test entry points must not be treated as current functionality.
- Status and progress: `status/` is the sole location for progress, known issues, module status and measurement protocols. `status/current/` describes the current state. `status/gaps/` is the **sole backlog and gap tracker**: remove completed items; an empty tracker means the committed scope is complete. `status/history/` retains superseded conclusions, and `status/measurements/` holds baselines and protocols. See [docs-gates.md](status/current/docs-gates.md) for current documentation tooling and verification entry points. Status documents do not define interface contracts or record commit SHAs and individual run results.
- Research notes: `research/` contains A20OS research claims and designs (capability envelopes, budget capabilities and the formalization agenda). Items labeled "not started/in progress" are research plans, not system features.
- Historical archives: `archive/` holds frozen milestone audit snapshots that no longer track HEAD. PASS results, measurements and "verified" claims apply only to their stated historical points.
- Third-party material: vendor documentation under `kernel/external/` and `user/external/` explains upstream context and does not define current A20OS interfaces.
- Conflict resolution order: HEAD source and headers, then the root `Makefile` and `tools/*.mk`/test scripts, then runtime logs matching the current commit, and finally narrative documentation.

## Choose a reference by role

User-facing procedures contain purpose, prerequisites, commands, observable outcomes and failure semantics. Lock ordering, references and ownership, internal state and rollback mechanisms belong in technical references or source comments; procedures link to them for readers who need implementation details. Register and DMA contracts in driver tutorials, and parameter layouts needed for debugging commands, are prerequisites for those tasks.

- Running the system: start with [building](dev/build.md), [instances](dev/instances.md), [packaging quick start](packaging/quickstart.md), [translator usage](kernel/exec-xlator/01-usage.md), [VM usage](hypervisor/03-usage.md) or [cluster operations](cluster/10-operator-guide.md).
- Remote page sharing: [configurable remote memory](cluster/runtime-ssi.md#2-可配置远程内存与页共享) provides explicit snapshots and transactions. See [transparent thread slices](cluster/runtime-ssi.md#3-透明线程执行片-thread-slices) for ordinary cross-machine execution; this is currently an experimental serial-handoff mode without same-process parallel acceleration across machines.
- Understanding the implementation: consult subsystem contracts such as [scheduling](kernel/process-scheduler.md), [network locks](net/network-lock-contract.md), [translator internals](kernel/exec-xlator/03-internals.md) and [driver runtime contracts](drivers/guide/runtime-contracts.md).
- Checking documentation: run `make check-docs` to check formatting, code citations, 100% MkDocs navigation coverage and links/anchors. `mkdocs build` builds the website with strict internal-link and anchor checks; `make docs` generates the PDF reference manual. See [documentation validation status](status/current/docs-gates.md).

## Documentation freshness

To avoid maintaining a separate verification baseline on every page:

- Documents describe **interfaces and design contracts**. HEAD source is authoritative; when text disagrees with source, use the source and submit a documentation correction.
- A page's "last verified" date records when it was checked against source. An old date alone does not mean the content is invalid.
- Runtime conclusions and artifacts are not maintained as current documentation or stored with the repository. Before citing a conclusion as a current fact, rerun its entry point in [testing-gates.md](testing-gates.md) on the **current commit**. Frozen milestone snapshots reside in `docs/zh/archive/`.

The following paths are useful for readers new to the project.

## Quick start for new contributors

If you are starting to read code, fix a bug or submit a patch:

- [architecture.md](architecture.md): overall architecture, dual ABIs and module organization.
- [dev/instances.md](dev/instances.md): declarative build/run/smoke instances (`instances/*.toml` and `tools/a20`) and component registries.
- [kernel/process-scheduler.md](kernel/process-scheduler.md): current process states, CPU ownership, Park/Wake, timeout, signal and SMP protocols.
- [testing-gates.md](testing-gates.md): local smoke tests and validation checks.
- [releases/v1.0.md](releases/v1.0.md): stable-release scope, subsystem completion criteria, implementation choices and interface freezes.
- [releases/v1.0-validation.md](releases/v1.0-validation.md): rebuildable references, behavioral acceptance, performance measurements and release blockers.
- [releases/v1.0-comparison.md](releases/v1.0-comparison.md): paired-comparison runner JSON protocol, statistical methods and evidence rules.
- [status/](status/README.md): the unified entry point for status, known issues, module state and measurement protocols.
- [releases/v0.17.md](releases/v0.17.md): v0.17 candidate scope, build fixes and release checks.
- [CONTRIBUTING.md](CONTRIBUTING.md): contribution workflow and code/comment conventions, including language, formatting, when to comment, lock ordering and ABI comments.
- [drivers/guide/getting-started.md](drivers/guide/getting-started.md): kernel integration through your first driver.
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md): acknowledged improvements and contribution opportunities.
- [status/current/server-readiness.md](status/current/server-readiness.md): read this before treating A20OS as a server; it describes remaining work directly.

## Driver development

For adding or maintaining a device driver:

- [drivers/README.md](drivers/README.md): driver manual entry point.
- [drivers/guide/core-model.md](drivers/guide/core-model.md): core device, driver and bus models.
- [drivers/guide/runtime-contracts.md](drivers/guide/runtime-contracts.md): MMIO, IRQ and DMA runtime contracts.
- [drivers/guide/pci-and-virtio.md](drivers/guide/pci-and-virtio.md): PCI and VirtIO integration.
- [drivers/classes/display.md](drivers/classes/display.md): framebuffer and display devices.
- [graphics/3d-graphics.md](graphics/3d-graphics.md): virtio-gpu 3D stack, kernel interfaces and status.
- [graphics/gpu-3d-roadmap.md](graphics/gpu-3d-roadmap.md): the route to running stock Mesa and displaying pixels, including VIRTGPU UAPI, ABI checks, ordering arguments and non-goals.
- [graphics/host-tools.md](graphics/host-tools.md): host tools `tools/check-drm-abi.sh` (DRM UAPI validation), `tools/build-virglrenderer.sh` (requires root, **not yet run**) and `tools/cli/a20_resource.py` (pre-launch resource checks).
- [graphics/real-hardware-gpu.md](graphics/real-hardware-gpu.md): physical GPU boundaries; scanout (2–4 weeks) and 3D acceleration (not feasible) are separate tasks, with measured driver sizes and per-SoC conclusions.
- [drivers/classes/audio.md](drivers/classes/audio.md): common audio UAPI, HDA, virtio-sound and PC Speaker.

## Platforms and porting

- [Platform documentation conventions](platforms/README.md): page scope, support status and operational requirements.
- [Porting guide](platforms/porting-guide.md): board interfaces, build integration and validation requirements.
- `tools/a20 boards` and `tools/a20 list`: query board configurations and instances in the current source. Deployment steps belong in each platform's document.

## User space, desktop and distributions

- [packaging/overview.md](packaging/overview.md): package management and image distribution, including apk packaging, world assembly, repository signing and CI/CD.
- [distro/README.md](distro/README.md): the Alpine rootfs path, using A20OS as the kernel and a native distribution as user space.
- [distro/source-software-porting.md](distro/source-software-porting.md): integrating software; put upstream Alpine packages in world manifests. Only `user/cmds/` and fastfetch still require source builds.

## Real-time operating system (RTOS)

CMSIS-RTOS2 is a separate third-party source-level ABI, not part of the Native ABI. See the [real-time execution and RTOS subsystem](rtos/README.md).

- [rtos/overview.md](rtos/overview.md): **A20OS real-time execution model and system architecture**.
- [rtos/cmsis-rtos2.md](rtos/cmsis-rtos2.md): third-party ABI positioning, API coverage, Native ABI surface and trimming.
- [rtos/execution-model.md](rtos/execution-model.md): thread mapping, priority buckets, two safe points, preemption, ticks and scheduler locking.
- [rtos/synchronization.md](rtos/synchronization.md): synchronization objects, PI wait-chain traversal, kernel donation and baseline snapshot restoration.
- [rtos/interrupts.md](rtos/interrupts.md): user-space ISR restrictions, vendor Dispatcher threads and deferred delivery.
- [rtos/porting-guide.md](rtos/porting-guide.md): minimal applications, tick alignment, stack sizing and LiteOS API comparison.
- [rtos/experiments.md](rtos/experiments.md): experiment suites 4.1–4.5, QEMU/physical-board runs and negative controls.
- [rtos/latency.md](rtos/latency.md): real-time latency derivations and plans for free-running-counter measurements.
- [kernel/process-scheduler.md](kernel/process-scheduler.md): kernel scheduling contracts, including RT buckets, EEVDF, persistent preemption, RT PI and scheduler locking.

## Native ABI and subsystem details

For deeper implementation details:

- [hybrid-kernel/00-design.md](hybrid-kernel/00-design.md): architectural form and layering principles.
- [hybrid-kernel/01-mechanisms.md](hybrid-kernel/01-mechanisms.md): hybrid-kernel semantics and contracts, including channel_call, svcmgr, shared rings, user-space drivers, file system protocols and vDSO.
- [hybrid-kernel/02-mainstream-plan.md](hybrid-kernel/02-mainstream-plan.md): gaps relative to mainstream hybrid kernels and future directions.
- [hybrid-kernel/06-user-fs.md](hybrid-kernel/06-user-fs.md): user-space file system hosting through uxfs and ufsd backends (fat/ext4/iso9660/ntfs-ro).
- [status/current/hybrid-kernel-status.md](status/current/hybrid-kernel-status.md): capabilities and boundaries.
- [kernel/process-scheduler.md](kernel/process-scheduler.md): process lifetimes, per-CPU runqueues, persistent preemption and blocking protocols.
- [native-abi/00-overview.md](native-abi/00-overview.md): Native ABI design overview.
- [native-abi/01-types.md](native-abi/01-types.md): basic types and syscall argument structures.
- [native-abi/03-handle.md](native-abi/03-handle.md): handles and object types.
- [native-abi/04-memory.md](native-abi/04-memory.md): VMO/VMAR memory model.
- [native-abi/05-ipc.md](native-abi/05-ipc.md): Channels and Event Queues.
- [fs/vfs-edge-semantics.md](fs/vfs-edge-semantics.md): VFS edge semantics.
- [net/network-lock-contract.md](net/network-lock-contract.md): network locking contracts.
- [net/network-config-design.md](net/network-config-design.md): network configuration (`a20.*` keys, the two `a20.tcpmode` modes and the runtime control plane).
- [net/net-lanes.md](net/net-lanes.md): network lanes and phase ordering; phases A/B are implemented, including an explanation of why locking contracts block the buffer redesign.

### Cross-architecture execution (exec-xlator)

When `execve` receives a foreign-architecture (x86_64 / aarch64) Linux executable, the kernel rewrites it into an in-place re-exec of a user-space translator. Choose an entry point by role:

- [kernel/exec-xlator/README.md](kernel/exec-xlator/README.md): mechanism and a three-minute quick start.
- [kernel/exec-xlator/01-usage.md](kernel/exec-xlator/01-usage.md): usage, three layers of switches, boot keys, argv templates, `/proc/a20/xlator`, verification without a cross compiler and troubleshooting.
- [kernel/exec-xlator/02-integration.md](kernel/exec-xlator/02-integration.md): translator contracts, changes needed to add a guest architecture, and feasibility of Rosetta-style wrappers and Prism-style AOT.
- [kernel/exec-xlator/03-internals.md](kernel/exec-xlator/03-internals.md): design choices, code map, `xlator_*` API contracts, concurrency invariants and known costs.

### Linux ABI compatibility layer

- `kernel/abi/linux/syscall_coverage.md`: per-syscall compatibility and evidence, including syscall-number coverage for the four main architectures.
- `kernel/abi/linux/compat_notes.md`: compatibility notes, including high-risk partial areas, file-based interfaces and placeholder decisions.
- [architecture.md](architecture.md): ABI layering (core implementations and thin wrappers) and comparison of the two ABIs.

### Virtualization (hypervisor)

The shared VM/vCPU core has RV64 H, RV32 H, AArch64 EL2, ARM32 HYP, AMD SVM and Intel VMX backends. Parallel execution of small payloads on two vCPUs has been verified across architectures. Intel VMX uses Bochs software CPUs and has not been verified on Intel hardware. LoongArch LVZ is experimental and disabled by default; only compilation and negative controls without LVZ have passed, with no actual LVZ guest execution evidence. Full-OS guest ELF/FDT loading and manager workflows still target RV64. Full-OS SMP, virtual GIC/APIC and long-term stability have not yet passed acceptance.

- [hypervisor/README.md](hypervisor/README.md): navigation, current backends and acceptance boundaries.
- [hypervisor/03-usage.md](hypervisor/03-usage.md): dependencies, building, QEMU / Bochs, bootargs, log criteria, minimal user-space guests, VMM/SMP call ordering and troubleshooting.
- [hypervisor/08-multiarch-backends.md](hypervisor/08-multiarch-backends.md): guest modes, page tables, registers and device boundaries per backend.
- [hypervisor/09-smp-validation.md](hypervisor/09-smp-validation.md): actual runtime matrix, reproduction commands, SMP methodology and image/log hashes.
- [status/current/hypervisor-implementation-status.md](status/current/hypervisor-implementation-status.md): implementation status, full-guest scenarios and incomplete work.
- [hypervisor/06-host-runtime.md](hypervisor/06-host-runtime.md), [hypervisor/07-guest-supervisor.md](hypervisor/07-guest-supervisor.md): named RV64 instances, disk/console isolation, dual-guest management and control protocols.
- [hypervisor/00-design.md](hypervisor/00-design.md), [hypervisor/01-a20os-guest.md](hypervisor/01-a20os-guest.md), [hypervisor/02-roadmap.md](hypervisor/02-roadmap.md), [hypervisor/04-platform-design.md](hypervisor/04-platform-design.md): foundational design, historical phases and future platform goals. Use current usage guides and runtime records for status.

The multi-architecture runtime entry point is `python3 tools/qemu/check_hyp_multiarch.py --arch ARCH --cpus 2 --build`; VMX uses a separate Bochs runner. Existing `smoke-hyp-*` checks retain RV64 full-guest coverage. Passing small multi-architecture payloads does not establish that all full-guest scenarios pass. Native and Linux extension ABIs share the core but have independent numbers, handles and permission wrappers. A20OS currently has no `/dev/kvm` compatibility layer.

## Current contracts and boundary records

- [kernel/mm/fork-mprotect-cow.md](kernel/mm/fork-mprotect-cow.md): current locking and frame-reference contracts for fork, COW and `mprotect`, plus isolation-regression scope.
- [kernel/arch/aarch64-fex-preconditions.md](kernel/arch/aarch64-fex-preconditions.md): AArch64 auxv and protection-hint interface audit scope, including which evidence does not establish FEX support.

## Research and project background

For research ideas and engineering roadmaps:

- [research/00-index.md](research/00-index.md): research index and arguments, with capability envelopes as the core contribution.
- [roadmap/a20os-improvement-todo.md](roadmap/a20os-improvement-todo.md): current improvements and development roadmap.
- [status/README.md](status/README.md): organization and index for status, gaps, history and measurement protocols.
- [status/current/server-readiness.md](status/current/server-readiness.md): server deployment boundaries, structural limitations and prioritized blockers.
- [security/hardening.md](security/hardening.md): security hardening status, including the fail-closed honesty principle.
- [dev/external-dependencies.md](dev/external-dependencies.md): external dependencies and licensing.
- [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md): third-party acknowledgments and attribution.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): centralized third-party license notices.
- `docs/zh/archive/`: frozen historical milestone audit snapshots.

---

These documents include current interfaces, research designs and historical records. Before citing a capability or test conclusion, classify it using the scope and freshness rules above.
