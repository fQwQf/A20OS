# Host-side unit tests for pure kernel format/helper logic.  These compile
# shared headers with the host compiler and run on the build machine, so
# format regressions are caught without booting the kernel.
HOST_CC ?= $(if $(filter llvm,$(TOOLCHAIN)),$(CLANG) $(LLVM_LINK),gcc)
HOST_CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra

# Every tools/tests/*.c falls into exactly one of three sets, and only the
# first goes through the generic single-file pattern rule below:
#
#   runnable   -- self-contained, builds with $(HOST_CC) $(HOST_CFLAGS)
#                 -Ikernel/include -Ikernel and has a main().  Default.
#   PROBES     -- no main(); compiled only to expose sizeof/offsetof through
#                 nm, or cross-compiled for another ABI.
#   SCRIPTED   -- needs something the pattern rule cannot supply: a module's
#                 Rust object, a per-arch include path, or a generated header.
#                 Each is built and run by the tools/test-*-host.sh script
#                 named beside it, and check-host-scripts runs all of them.
#
# The SCRIPTED list is explicit rather than a glob so a new self-contained
# harness is still discovered automatically.  Leaving a harness out of either
# list does not silently drop it from the gate: it lands in "runnable" and
# fails the build, which is how these lists were found to be incomplete.
HOST_TESTS_PROBES := \
    tools/tests/fdt_view.c \
    tools/tests/eevdf_node_layout_probe.c \
    tools/tests/rust_rv32_ilp32d_abi.c \
    tools/tests/rust_rv32_sync_abi.c \
    tools/tests/vfs_shutdown_layout.c

# Template fixtures consumed by a Python unittest: the test substitutes
# production code into the marker comments before compiling, so the file is
# not a standalone harness.
HOST_TESTS_FIXTURES := \
    tools/tests/anon_child_mapper_host.c

# Harnesses that need a dedicated runner, or that still need updating after the
# net/SSI refactor.  They are excluded from the generic pattern rule; several
# are exercised by their own make targets or scripts (check-cluster-unit,
# check-native-spawn-admission, tools/tests/net_backend_host.sh,
# tools/host-tests/test-bsd-runtime-host.py).
HOST_TESTS_UNBUILT := \
    tools/tests/bsd_clock_host.c \
    tools/tests/bsd_memory_host.c \
    tools/tests/cow_clone_range_host.c \
    tools/tests/cow_snapshot_token_host.c \
    tools/tests/drm_scanout_mapping_host.c \
    tools/tests/drm_scanout_host.c \
    tools/tests/hyp_s2_host.c \
    tools/tests/hyp_sbi_host.c \
    tools/tests/mincore_file_cache_host.c \
    tools/tests/mmtlb_hold_release_host.c \
    tools/tests/net_backend_host.c \
    tools/tests/procfs_diag_snapshot_host.c \
    tools/tests/pt_cache_owner_host.c \
    tools/tests/pt_map_owner_host.c \
    tools/tests/test_clx_board_uart.c \
    tools/tests/test_clx_memory.c \
    tools/tests/test_native_spawn_admission.c \
    tools/tests/x86_pte_block_encoder_host.c

# SCRIPTED, grouped by the driver script that runs them.
HOST_TESTS_SCRIPTED := \
    tools/tests/test_acpi_tables.c \
    tools/tests/test_uacpi_namespace.c \
    tools/tests/uacpi_host_test_osl.c \
    tools/tests/host_sunxi_mmc.c \
    tools/tests/block_cache_state_host.c \
    tools/tests/rtw89_runtime_host.c \
    tools/tests/completion_state_host.c \
    tools/tests/envelope_policy_host.c \
    tools/tests/ext4_sync_c_scope_host.c \
    tools/tests/ext4_sync_callback_host.c \
    tools/tests/mntns_lifecycle_host.c \
    tools/tests/mount_state_host.c \
    tools/tests/mutate_state_host.c \
    tools/tests/page_cache_state_host.c \
    tools/tests/path_state_host.c \
    tools/tests/path_walk_state_host.c \
    tools/tests/procfs_node_state_host.c \
    tools/tests/procfs_state_host.c \
    tools/tests/rt_throttle_accounting.c \
    tools/tests/rw_mutex_state_host.c \
    tools/tests/test_bcache_flush_adapter.c \
    tools/tests/test_bcache_user_flush.c \
    tools/tests/test_cow_clone.c \
    tools/tests/test_cow_fault.c \
    tools/tests/test_cow_status.c \
    tools/tests/test_fault_status.c \
    tools/tests/test_madvise_policy.c \
    tools/tests/test_mempolicy_state.c \
    tools/tests/test_mmap_policy.c \
    tools/tests/test_mprotect_txn.c \
    tools/tests/test_mremap_dontunmap_txn.c \
    tools/tests/test_mseal_policy.c \
    tools/tests/test_munmap_txn.c \
    tools/tests/test_oom_policy.c \
    tools/tests/test_swap_slots.c \
    tools/tests/test_uxfs_flush_client.c \
    tools/tests/test_vmar_policy.c \
    tools/tests/test_wx_aslr_policy.c \
    tools/tests/ufs_close_token_host.c \
    tools/tests/uxfs_lifetime_state_host.c \
    tools/tests/uxfs_rpc_state_host.c \
    tools/tests/vfile_lifecycle_host.c \
    tools/tests/vfs_shutdown_abi_host.c \
    tools/tests/vnode_identity_bench.c \
    tools/tests/vnode_identity_host.c \
    tools/tests/wait4_host.c \
    tools/tests/wait_queue_state_host.c \
    tools/tests/wait_timer_host.c

HOST_TESTS_SRC := $(filter-out $(HOST_TESTS_PROBES) $(HOST_TESTS_SCRIPTED) \
                               $(HOST_TESTS_FIXTURES) $(HOST_TESTS_UNBUILT),\
                               $(wildcard tools/tests/*.c))
HOST_TESTS_BIN := $(patsubst tools/tests/%.c,$(TMPDIR)/a20-host-%,$(HOST_TESTS_SRC))
HOST_TESTS_BIN += $(TMPDIR)/a20-host-test_lwip_allocator_single_lane

# Production firmware/provider/driver graph fixtures, including the real AML
# interpreter. Python owns their multi-source host compilation and UBSan runs.
# Reusable PC driver contracts; no block-device writes or physical reset.
.PHONY: check-pc-drivers
check-pc-drivers: check-component-registry
	$(PYTHON) -m unittest tools.tests.test_i2c_transport_lifetime \
	  tools.tests.test_firmware_gpio_input tools.tests.test_pc_network_lifetime \
	  tools.tests.test_pc_module_boundary \
	  tools.tests.test_xhci_endpoint_types \
	  tools.tests.test_xhci_event_owner_race tools.tests.test_xhci_firmware_policy \
	  tools.tests.test_usb_hid_protocol tools.tests.test_usb_hid_readiness \
	  tools.tests.test_usb_hid_console tools.tests.test_usb_keyboard_leds \
	  tools.tests.test_input_mt_state tools.tests.test_evdev_mt_libevdev \
	  tools.tests.test_wifi_broker tools.tests.test_wifi_gui \
	  tools.tests.test_wifi_hardware_address tools.tests.test_wifi_connect_reuse \
	  tools.tests.test_rtw89_runtime tools.tests.test_drm_console_owner \
	  tools.tests.test_desktop_session_log tools.tests.test_session_power \
	  tools.tests.test_laptop_recovery

.PHONY: check-firmware-model
check-firmware-model:
	$(PYTHON) -m unittest tools.tests.test_firmware_model \
	  tools.tests.test_devicetree tools.tests.test_acpi_tables \
	  tools.tests.test_uacpi_namespace tools.tests.test_pci_resources \
	  tools.tests.test_e1000_interrupt_setup tools.tests.test_ahci_pci_policy \
	  tools.tests.test_xhci_firmware_policy tools.tests.test_usb_hid_readiness \
	  tools.tests.test_xhci_endpoint_types \
	  tools.tests.test_alsa_lib_protocol \
	  tools.tests.test_hda_pcm_readiness \
	  tools.tests.test_arch_layout tools.tests.test_arch_mmio \
	  tools.tests.test_arch_fdt_memory tools.tests.test_board_module_devices \
	  tools.tests.test_pine_a64_fdt tools.tests.test_quartz64_zero_fdt \
	  tools.tests.test_lichee_pi_zero_fdt tools.tests.test_riscv_fdt_memory \
	  tools.tests.test_ssi_arch_state tools.tests.test_elf_load_span \
	  tools.tests.test_mcu_heap_backend tools.tests.test_x86_boot_memory \
	  tools.tests.test_x86_baytrail_tsc \
	  tools.tests.test_drm_scanout tools.tests.test_xfce_renderer \
	  tools.tests.test_drm_scanout_mapping \
	  tools.tests.test_aarch64_uefi_memory tools.tests.test_aarch64_uefi_loader \
	  tools.tests.test_class_binding tools.tests.test_native_bus_lifetime \
	  tools.tests.test_driver_remove tools.tests.test_native_bus_remove \
	  tools.tests.test_sunxi_mmc tools.tests.test_sdhci \
	  tools.tests.test_dw_sdio tools.tests.test_ls2k0300_liointc \
	  tools.tests.test_board_shell_profiles tools.tests.test_pc_package_paths \
	  tools.tests.test_serial_shell_smoke \
	  tools.tests.test_arm32_poll_abi tools.tests.test_fault_ffi_width \
	  tools.tests.test_vma_release_ffi_width \
	  tools.tests.test_boot_framebuffer \
	  tools.tests.test_dt_framebuffer tools.tests.test_simplefb_lifecycle \
	  tools.tests.test_lsdc_boot tools.tests.test_ls2k0300_scanout \
	  tools.tests.test_quartz64_zero_video \
	  tools.tests.test_sunxi_audio \
	  tools.tests.test_rk817_audio \
	  tools.tests.test_board_audio_enumeration \
	  tools.tests.test_sg2002_audio tools.tests.test_riscv_dma_cache \
	  tools.tests.test_dma_arch_readiness \
	  tools.tests.test_module_clock \
	  tools.tests.test_videoplay tools.tests.test_media_mount \
	  tools.tests.test_ramfs_distro_mount \
	  tools.tests.test_uefi_esp_image tools.tests.test_uefi_world \
	  tools.tests.test_memcpy_boundaries
	$(PYTHON) -m unittest tools.tests.test_sleep_abi

.PHONY: host-tests check-instances check-instance-matrix \
        check-component-registry regen-driver-fragment \
        check-trim-registry regen-trim-fragment \
        check-smoke-cases \
        check-flash-backend-registry check-manifests \
        check-a20-tests check-pt-mcs-preempt-window check-proc-wait-window \
        check-tg-leader-lifetime

# Instance/manifest gates (docs/dev/instances.md).  check-instance-matrix pins the
# hosted-arch matrix: every SUPPORTED_HOSTED_ARCHES member must be covered by
# at least one valid instance, so the matrix and instances/ cannot drift.
check-instances:
	@tools/a20 check

check-instance-matrix:
	@tools/a20 check --require-arch $(SUPPORTED_HOSTED_ARCHES)

check-component-registry:
	@tools/a20 check-registry

# components/drivers.mk is generated from components/drivers.toml.  Run this
# after editing the TOML; check-component-registry fails while it is stale, so
# the generated copy can never rot the way a second hand-maintained list could.
# Every smoke case must be reachable as a make target, and every target that
# calls tools/smoke.py must name a case that exists.  One make spawn.
check-smoke-cases:
	@$(PYTHON) tools/smoke_audit.py

regen-driver-fragment:
	@tools/a20 regen-drivers

# components/trim.mk is generated from components/trim.toml -- the policy half
# of kernel trimming (profile source lists, capability arch matrices).  Same
# two-layer stance as the driver registry: the TOML is self-validated, the
# generated fragment must not go stale, and every emitted variable must still
# have a makefile consumer so no entry can die silently.
check-trim-registry:
	@tools/a20 check-trim

regen-trim-fragment:
	@tools/a20 regen-trim

# Flash-programmer registry (components/flash-backends.toml).  Two layers, like
# the driver registry: the toml is self-validated (unique names, boards that
# exist, targets that exist as make rules), then every make target is
# cross-checked against the set a20 can actually dispatch to.  Neither side can
# drift without FAIL.
check-flash-backend-registry:
	@tools/a20 check-flash-backends

# Aggregate for the manifest-layer gates.  These are host-side, pure-Python and
# arch-independent, so they are cheap enough to always run: instances/,
# components/ and the Makefile must never drift apart.  Wired into CI as the
# `manifest-gates` job.
check-manifests: check-instances check-instance-matrix check-component-registry \
                 check-trim-registry check-flash-backend-registry
	@echo "check-manifests: PASS"

# Unit tests for the a20 toolchain itself (tools/tests/test_a20.py).  Stdlib
# unittest, matching the tool's zero-dependency promise.  Runs from a checkout
# with no submodules, same as the manifest gates.
#
# user/svc/a20_services_idl.h is a prerequisite, not an accident: the header is
# generated and gitignored, and test_a20idl.py compiles probes that include it.
# Without the prerequisite the gate only passed on a tree where some earlier
# target had happened to generate it, so it failed on every clean checkout --
# including CI's toolchain-gates job, which runs nothing else that would create
# it.  The literal path (not $(A20_SERVICES_IDL_HDR)) is deliberate for the same
# reason as the IDL wire rule below: this file is included before
# tools/mk/targets-native.mk defines that variable.
check-a20-tests: user/svc/a20_services_idl.h
	@$(PYTHON) -m unittest discover --start-directory tools/tests --pattern 'test_*.py'
	@echo "check-a20-tests: PASS"
# DRM ioctl numbers and wire struct layouts vs the Linux UAPI.  A wrong number
# never matches the dispatch switch, so the ioctl falls through to the default
# arm and userspace sees EINVAL/ENOTTY -- which reads as a Mesa or libdrm bug
# rather than as a wrong constant in a header.  Gate it so that class of
# mistake cannot land again.  Skips where no UAPI headers are installed.
.PHONY: check-drm-abi
check-drm-abi:
	@tools/check-drm-abi.sh

host-tests: check-pt-mcs-preempt-window check-proc-wait-window \
            check-wait4-controller check-tg-leader-lifetime check-fdt-view \
            check-host-scripts $(HOST_TESTS_BIN)
	@set -e; for t in $(HOST_TESTS_BIN); do echo "== $$t =="; "$$t"; done
	@echo "host-tests: PASS"

# The harnesses in HOST_TESTS_SCRIPTED are not built by the pattern rule; run
# their driver scripts so being outside HOST_TESTS_BIN does not mean being
# outside the gate.
check-host-scripts:
	@tools/run-host-scripts.sh

$(TMPDIR)/a20-host-%: tools/tests/%.c
	$(HOST_CC) $(HOST_CFLAGS) -Ikernel/include -Ikernel $< -o $@

# The IDL wire tests need the Native SDK type header, which the generic rule's
# -Ikernel/include -Ikernel does not reach, and a generated IDL header.  The
# literal user/svc/a20_services_idl.h (not $(A20_SERVICES_IDL_HDR)) is
# deliberate: this file is included before tools/mk/targets-native.mk defines it.
# HOST_TESTS_SRC is a $(wildcard tools/tests/*.c), so both binaries below are
# already in HOST_TESTS_BIN and `make host-tests` runs them; the explicit rules
# exist only to override the pattern rule's include path and to generate the
# mixed-width header first.
#
# test_a20_idl_wire pins the v1 bytes (the shipped header).
# test_a20_idl_mixed compiles a header generated from a synthetic IDL whose
# message is NOT packed on any target, which is the only way to catch a codec
# that quietly defers to host struct layout -- and, before this existed, the
# only way to notice a u8 field that emitted non-C.  tools/tests/idl_mixed.idl
# has a .idl suffix so the wildcard above does not try to compile it as a test.
$(TMPDIR)/a20idl_mixed.h: tools/tests/idl_mixed.idl tools/a20idl.py
	$(PYTHON) tools/a20idl.py tools/tests/idl_mixed.idl $@

$(TMPDIR)/a20-host-test_a20_idl_wire: tools/tests/test_a20_idl_wire.c user/svc/a20_services_idl.h user/svc/a20_services.idl tools/a20idl.py
	$(HOST_CC) $(HOST_CFLAGS) -Iuser/liba20rt -Iuser/svc $< -o $@

$(TMPDIR)/a20-host-test_a20_idl_mixed: tools/tests/test_a20_idl_mixed.c $(TMPDIR)/a20idl_mixed.h
	$(HOST_CC) $(HOST_CFLAGS) -Iuser/liba20rt -I$(TMPDIR) $< -o $@

# The flat-space policy is Rust, so this harness links the Rust object; the C
# file provides only the bridges.  During the takeover the same harness was also
# compiled against the C implementation and both sides passed
# (tools/host-tests/test-flat-policy-host.sh, .kernel-build/evidence/fl-slice-evidence.txt).
$(TMPDIR)/a20-host-test_flat_user: tools/tests/test_flat_user.c kernel/mm/flat_user.c tools/tests/flat_user_shim.h kernel/mm/flat_policy.rs
	rustc --edition=2021 --crate-type lib --emit=obj --target x86_64-unknown-linux-gnu \
		-C panic=abort -C opt-level=1 kernel/mm/flat_policy.rs -o $(TMPDIR)/flat_policy_host.o
	$(HOST_CC) $(HOST_CFLAGS) -pthread -DA20_FLAT_HOST_TEST -Ikernel/include \
		-include tools/tests/flat_user_shim.h $(filter %.c,$^) $(TMPDIR)/flat_policy_host.o -o $@

$(TMPDIR)/a20-host-test_native_runtime: tools/tests/test_native_runtime.c kernel/core/runtime.c kernel/abi/native/sys_runtime.c tools/tests/native_runtime_shim.h tools/tests/native_task_object_shim.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -Ikernel/include -Ikernel \
		-include tools/tests/native_runtime_shim.h $(filter %.c,$^) -o $@

$(TMPDIR)/a20-host-test_native_task_object: tools/tests/test_native_task_object.c kernel/proc/task_object.c kernel/proc/pid.c kernel/ipc/a20_object.c tools/tests/native_task_object_shim.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -Ikernel/include -Ikernel \
		-include tools/tests/native_task_object_shim.h $(filter %.c,$^) -o $@

$(TMPDIR)/a20-host-test_native_spawn_admission: tools/tests/test_native_spawn_admission.c kernel/abi/native/sys_spawn_flat.c tools/tests/native_spawn_shim.h
	$(HOST_CC) $(HOST_CFLAGS) -DCONFIG_NATIVE_FLAT_FILES -Ikernel/include -Ikernel/abi/native \
		-include tools/tests/native_spawn_shim.h $(filter %.c,$^) -o $@

$(TMPDIR)/a20-host-test_native_thread_admission: tools/tests/test_native_thread_admission.c kernel/abi/native/sys_thread_create.c tools/tests/native_thread_shim.h
	$(HOST_CC) $(HOST_CFLAGS) -Ikernel/include -Ikernel/abi/native \
		-include tools/tests/native_thread_shim.h $(filter %.c,$^) -o $@

# The MCU protocol harness uses architecture type definitions but executes
# no target assembly. The generic host rule lacks this include directory.
$(TMPDIR)/a20-host-test_clx_fragment $(TMPDIR)/a20-host-test_mcu_leaf $(TMPDIR)/a20-host-test_clx_uart_pair $(TMPDIR)/a20-host-test_clx_oneway $(TMPDIR)/a20-host-test_clx_fuzz: $(TMPDIR)/a20-host-%: tools/tests/%.c
	$(HOST_CC) $(HOST_CFLAGS) -Ikernel/arch/riscv64/include \
		-Ikernel/include -Ikernel $< -o $@

$(TMPDIR)/a20-host-test_clx_oneway: tools/tests/test_clx_uart_pair.c kernel/mcu/leaf.c kernel/cluster/uart.c

.PHONY: check-cluster-unit check-cluster-udp check-cluster-loopback check-cluster-reliable
check-cluster-unit:
	@$(PYTHON) tools/cluster/check-cluster-unit.py

check-cluster-udp:
	@$(PYTHON) tools/cluster/check-cluster-udp.py

check-cluster-loopback:
	@$(PYTHON) tools/cluster/check-cluster-loopback.py

# Exercise the actual vendored lwIP heap and pbuf code under host threads. The
# host ports supply architecture types, CPU identity and lock shims; allocator and
# reference-count algorithms come from the kernel's lwIP sources.
$(TMPDIR)/a20-host-test_lwip_allocator_concurrency \
$(TMPDIR)/a20-host-test_lwip_allocator_single_lane: \
        tools/tests/test_lwip_allocator_concurrency.c \
        kernel/net/backends/native/protocol/src/core/mem.c \
        kernel/net/backends/native/protocol/src/core/memp.c \
        kernel/net/backends/native/protocol/src/core/pbuf.c \
        kernel/net/backends/lwip/lwip_compat.c \
        kernel/net/backends/lwip/port/arch/heap.h \
        tools/tests/lwip_host_port/core/lock.h \
        tools/tests/lwip_host_port/arch/cc.h \
        tools/tests/lwip_host_port/lwipopts.h \
        tools/tests/net_lane_host_port/core/cpu.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -ffunction-sections -fdata-sections \
	  -DCONFIG_X86_64 -DCONFIG_NR_CPUS=9 \
	  -DCONFIG_NET_LANES=$(if $(findstring single_lane,$@),1,4) \
	  -Itools/tests/lwip_host_port \
	  -Itools/tests/net_lane_host_port \
	  -Ikernel/net/backends/native/protocol/src/include -Ikernel/include -Ikernel \
	  $(filter %.c,$^) -Wl,--gc-sections -o $@

# Compile the real lane queue implementation, with only CPU identity shimmed.
$(TMPDIR)/a20-host-test_net_lane_concurrency: tools/tests/test_net_lane_concurrency.c \
        kernel/net/backends/lwip/net_lane.c kernel/include/net/net_lane.h kernel/include/net/profile.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -DCONFIG_NET_LANES=4 -DCONFIG_NR_CPUS=4 \
	  -Itools/tests/net_lane_host_port -Ikernel/include -Ikernel \
	  tools/tests/test_net_lane_concurrency.c kernel/net/backends/lwip/net_lane.c -o $@

# Exercise the real spin_lock_at() inline body.  The host exchange seam injects
# a simulated IRQ at lock acquisition and verifies that preemption was already
# disabled, while the contention seam verifies failed waiters remain switchable.
$(TMPDIR)/a20-host-test_spinlock_preempt_window: \
        tools/tests/test_spinlock_preempt_window.c \
        kernel/core/preempt.c \
        kernel/include/core/lock.h \
        tools/tests/lock_host_port/core/types.h \
        tools/tests/lock_host_port/core/consts.h \
        tools/tests/lock_host_port/core/defs.h \
        tools/tests/lock_host_port/core/klog.h \
        tools/tests/lock_host_port/core/cpu.h \
        tools/tests/lock_host_port/core/timer.h \
        tools/tests/lock_host_port/core/preempt.h
	$(HOST_CC) $(HOST_CFLAGS) -DCONFIG_KERNEL_PREEMPT=1 \
	  -Itools/tests/lock_host_port -Ikernel/include \
	  tools/tests/test_spinlock_preempt_window.c -o $@

# Extract and execute the production PT MCS lock bodies with a deterministic
# CPU-migration seam.  The negative control removes mcs_lock's pin and must
# trip the production non-LIFO guard on unlock.
check-pt-mcs-preempt-window:
	$(PYTHON) tools/tests/test_pt_mcs_preempt_window.py

# Extract and run proc_wait4 with deterministic child-exit timing seams.  This
# guards both wait registration lost-wakeup and zombie-leader/live-thread
# semantics without booting a guest.
check-proc-wait-window:
	$(PYTHON) tools/tests/test_wait_registration_window.py

# Run the wait4 decision scenario table (matching, reap/report classification,
# status encoding, transaction order) against the production Rust controller
# linked into the extracted C adapter.
check-wait4-controller:
	$(PYTHON) tools/tests/test_wait4_host.py

# Prove the task-group leader remains alive while linked members can still
# update its embedded tg_next chain, using the production link/unlink bodies.
check-tg-leader-lifetime:
	$(PYTHON) tools/tests/test_tg_leader_lifetime.py

# Minimal ISO9660 test image for the isofs driver (no mkisofs/xorriso needed).
$(ISOFS_IMG): tools/fs/mkisofs_test.c
	@mkdir -p $(BUILD_DIR) $(TMPDIR)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $(TMPDIR)/a20-mkisofs
	$(TMPDIR)/a20-mkisofs $@

check-abi-boundary:
	@$(PYTHON) tools/gen_linux_syscall_coverage.py --check

# ----------------------------------------------------------------
# `make check` -- the fast host-side test tier
# ----------------------------------------------------------------
# Every target CI's `host-tests` job runs, in the order the job runs them.
# Keeping the list in one variable (rather than as bare prerequisites) is what
# lets the recipe below report per-gate results and let `make help` print the
# same list; a second hand-typed copy is exactly how a gate stops being run
# locally while still passing in CI.
#
# The fast tier is host-side only: pure Python, the host gcc, and the kept
# behavioral harnesses.  It needs neither a cross toolchain nor QEMU, so
# `make check` is always runnable on a bare checkout.
CHECK_FAST_GATES := \
    check-manifests \
    check-a20-tests \
    check-smoke-cases \
    host-tests \
    check-cluster-unit \
    check-cluster-fuzz \
    check-cluster-wakeup \
    check-slab-cpu-window \
    check-virtio-net-completion \
    check-drm-abi \
    check-abi-boundary \
    test-nat-rewrite \
    test-signal-core \
    test-ckpt-image

# Recurse per gate instead of listing them as prerequisites.  A single make
# would run them in parallel under -j and stop at the first failure; one
# $(MAKE) per gate keeps the jobserver honest (so `make -j check` does not
# oversubscribe), keeps the order deterministic, and lets every gate run so a
# single `make check` reports *all* the drift in the tree rather than the first
# symptom of it.
.PHONY: check
check:
	@failed=''; \
	for gate in $(CHECK_FAST_GATES); do \
	  printf '\n=== %s ===\n' "$$gate"; \
	  $(MAKE) --no-print-directory $$gate || failed="$$failed $$gate"; \
	done; \
	printf '\n'; \
	if [ -n "$$failed" ]; then \
	  echo "check: FAIL --$$failed"; \
	  echo "  ($(words $(CHECK_FAST_GATES)) gates run; a full-fidelity local"; \
	  echo "   reproduction of CI's toolchain-gates job)"; \
	  exit 1; \
	fi; \
	echo "check: PASS -- all $(words $(CHECK_FAST_GATES)) host-side gates green"

# ----------------------------------------------------------------
# NAT rewrite host tests
# ----------------------------------------------------------------
# The SNAT/MASQUERADE half of netfilter has no end-to-end gate and cannot have
# one on QEMU's user-mode network: there is no peer behind the guest's own NAT to
# send a reply.  So the address rewrite, the port rewrite and the reply-direction
# match are asserted here, on the host, against the shipped C reference or the
# selected Rust implementation through its production C ABI shim. In `make
# check` because it takes about a second and needs no target runtime.
.PHONY: test-ckpt-image
test-ckpt-image:
	@sh tools/host-tests/test-ckpt-image-host.sh
	@echo "test-ckpt-image: PASS"

.PHONY: test-nat-rewrite
test-nat-rewrite:
	@sh tools/host-tests/test-nat-rewrite-host.sh $(NETFILTER_REWRITE_IMPL)
	@echo "test-nat-rewrite ($(NETFILTER_REWRITE_IMPL)): PASS"

# ----------------------------------------------------------------
# Signal decision-core host tests
# ----------------------------------------------------------------
# The signal core is pure decision logic, so its Rust unit/oracle vectors and
# the C<->Rust differential sweep run on the host in about a second and need no
# target runtime. Also re-runs the frozen Python/C oracle models.
.PHONY: test-signal-core
test-signal-core:
	@sh tools/host-tests/test-signal-core-host.sh
	@echo "test-signal-core: PASS"

check-cluster-reliable:
	$(PYTHON) tools/cluster/check-cluster-udp.py --faults --restart --log-dir .kernel-build/cluster/reliable

.PHONY: check-cluster-send check-cluster-smp
check-cluster-smp:
	NR_CPUS=2 $(PYTHON) tools/cluster/check-cluster-udp.py --send --faults --random-loss 0.1 --profile 3 --timeout 240 --log-dir .kernel-build/cluster/send-smp2

check-cluster-send:
	$(PYTHON) tools/cluster/check-cluster-udp.py --send --faults --random-loss 0.1 --profile 3 --timeout 240 --log-dir .kernel-build/cluster/send-server
	$(PYTHON) tools/cluster/check-cluster-udp.py --send --routed --faults --profile 3 --peer-profile 2 --timeout 180 --log-dir .kernel-build/cluster/send-routed-mixed
	$(PYTHON) tools/cluster/check-cluster-gateway.py --send --timeout 180

.PHONY: check-cluster-mesh
check-cluster-mesh:
	$(PYTHON) tools/cluster/check-cluster-mesh.py

.PHONY: check-cluster-fuzz
check-cluster-fuzz:
	$(PYTHON) tools/cluster/check-cluster-fuzz.py

.PHONY: check-cluster-fuzz-smoke check-cluster-performance
check-cluster-fuzz-smoke:
	$(PYTHON) tools/cluster/check-cluster-fuzz.py --seconds 300

check-cluster-performance:
	$(PYTHON) tools/cluster/check-cluster-performance.py

.PHONY: check-cluster-load
check-cluster-load:
	$(PYTHON) tools/cluster/check-cluster-load.py

.PHONY: check-virtio-net-completion
check-virtio-net-completion:
	$(PYTHON) tools/net/check-virtio-net-completion.py

.PHONY: check-cluster-wakeup
check-cluster-wakeup:
	$(PYTHON) tools/cluster/check-cluster-wakeup.py

.PHONY: check-slab-cpu-window
check-slab-cpu-window:
	$(PYTHON) tools/mm/check-slab-cpu-window.py

.PHONY: check-cluster-uart
check-cluster-uart:
	$(PYTHON) tools/cluster/check-cluster-uart.py

.PHONY: check-cluster-gateway
check-cluster-gateway:
	$(PYTHON) tools/cluster/check-cluster-gateway.py

.PHONY: demo-wordcount
demo-wordcount:
	$(PYTHON) tools/cluster/check-cluster-wordcount.py
	$(PYTHON) tools/cluster/check-cluster-wordcount.py --mixed

.PHONY: check-cluster-hardening
check-cluster-hardening:
	$(PYTHON) tools/cluster/check-cluster-udp.py --faults --restart --stress --random-loss 0.1 --corrupt --timeout 420 --log-dir .kernel-build/cluster/random-loss1000
	$(PYTHON) tools/cluster/check-cluster-udp.py --faults --restart --random-delay --corrupt --hello-flood --timeout 240 --log-dir .kernel-build/cluster/delay-crc-flood
	$(PYTHON) tools/cluster/check-cluster-udp.py --faults --partition --slow-consumer --timeout 150 --log-dir .kernel-build/cluster/partition-slow

.PHONY: check-cluster-instance
check-cluster-instance:
	$(PYTHON) tools/cluster/check-cluster-instance.py

.PHONY: check-cluster-objects
check-cluster-objects:
	$(PYTHON) tools/cluster/check-cluster-objects.py

.PHONY: check-cluster-apps
check-cluster-apps:
	$(PYTHON) tools/cluster/check-cluster-apps.py
