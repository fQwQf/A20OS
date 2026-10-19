# Host-side unit tests for pure kernel format/helper logic.  These compile
# shared headers with the host compiler and run on the build machine, so
# format regressions are caught without booting the kernel.
HOST_CC ?= gcc
HOST_CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra
HOST_TESTS_SRC := $(filter-out tools/tests/fdt_view.c,$(wildcard tools/tests/*.c))
# Rust-linked differential harnesses are not self-contained host programs: they
# need their module's staticlib and are driven by the dedicated tools/test-*.sh
# scripts (like the wait4/vfile_lifecycle harnesses).  Keep them out of the
# generic single-file host-tests pattern rule.
HOST_TESTS_SRC := $(filter-out tools/tests/vnode_identity_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/vnode_identity_bench.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/block_cache_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/page_cache_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/uxfs_rpc_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/uxfs_lifetime_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/procfs_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/path_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/path_walk_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/mutate_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/mntns_lifecycle_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/procfs_node_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_SRC := $(filter-out tools/tests/mount_state_host.c,$(HOST_TESTS_SRC))
HOST_TESTS_BIN := $(patsubst tools/tests/%.c,$(TMPDIR)/a20-host-%,$(HOST_TESTS_SRC))
HOST_TESTS_BIN += $(TMPDIR)/a20-host-test_lwip_allocator_single_lane

# Execute the production common fabric driver for x86-64, AArch64 and RV64.
.PHONY: check-shared-fabric-portability
check-shared-fabric-portability:
	$(PYTHON) tools/check-shared-fabric-portability.py

.PHONY: host-tests check-vfs-abstraction check-instances check-instance-matrix \
        check-component-registry regen-driver-fragment \
        check-trim-registry regen-trim-fragment \
        check-smoke-cases \
        check-native-abi-coverage check-abi-config-guard \
        check-flash-backend-registry check-manifests \
        check-a20-tests check-pt-mcs-preempt-window check-proc-wait-window \
        check-tg-leader-lifetime

# Instance/manifest gates (docs/instances.md).  check-instance-matrix pins the
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

# kernel/proc/xlator_guests.def and tools/targets-xlator.mk are the two halves
# of "which guest architectures exist", and neither can be derived from the
# other -- one is kernel policy, the other a fact about which cross compilers
# are installed.  They had already drifted once (a CC line for a guest the
# kernel never registered), so the link is asserted here rather than hoped
# for.  Host-side text only: no cross toolchain, no QEMU.
check-xlator-guests:
	@$(PYTHON) tools/xlator_guests_audit.py

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
check-a20-tests:
	@$(PYTHON) -m unittest discover --start-directory tools/tests --pattern 'test_*.py'
	@echo "check-a20-tests: PASS"
# DRM ioctl numbers and wire struct layouts vs the Linux UAPI.  A wrong number
# never matches the dispatch switch, so the ioctl falls through to the default
# arm and userspace sees EINVAL/ENOTTY -- which reads as a Mesa or libdrm bug
# rather than as a wrong constant in a header.  Gate it so that class of
# mistake cannot land again.  Skips where no UAPI headers are installed.
.PHONY: check-drm-abi check-drm-store-locking
check-drm-abi:
	@tools/check-drm-abi.sh

host-tests: check-pt-mcs-preempt-window check-proc-wait-window \
            check-tg-leader-lifetime check-fdt-view $(HOST_TESTS_BIN)
	@$(PYTHON) tools/gates.py host-tests --binaries "$(HOST_TESTS_BIN)"

$(TMPDIR)/a20-host-%: tools/tests/%.c
	$(HOST_CC) $(HOST_CFLAGS) -Ikernel/include -Ikernel $< -o $@

# The IDL wire tests need the Native SDK type header, which the generic rule's
# -Ikernel/include -Ikernel does not reach, and a generated IDL header.  The
# literal user/svc/a20_services_idl.h (not $(A20_SERVICES_IDL_HDR)) is
# deliberate: this file is included before tools/targets-native.mk defines it.
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
$(TMPDIR)/a20-idl-mixed.h: tools/tests/idl_mixed.idl tools/a20idl.py
	$(PYTHON) tools/a20idl.py tools/tests/idl_mixed.idl $@

$(TMPDIR)/a20-host-test_a20_idl_wire: tools/tests/test_a20_idl_wire.c user/svc/a20_services_idl.h user/svc/a20_services.idl tools/a20idl.py
	$(HOST_CC) $(HOST_CFLAGS) -Iuser/liba20rt -Iuser/svc $< -o $@

$(TMPDIR)/a20-host-test_a20_idl_mixed: tools/tests/test_a20_idl_mixed.c $(TMPDIR)/a20-idl-mixed.h
	$(HOST_CC) $(HOST_CFLAGS) -Iuser/liba20rt -I$(TMPDIR) $< -o $@

$(TMPDIR)/a20-host-test_flat_user: tools/tests/test_flat_user.c kernel/mm/flat_user.c tools/tests/flat_user_shim.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -DA20_FLAT_HOST_TEST -Ikernel/include \
		-include tools/tests/flat_user_shim.h $(filter %.c,$^) -o $@

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
	@$(PYTHON) tools/check-cluster-unit.py

check-cluster-udp:
	@$(PYTHON) tools/check-cluster-udp.py

check-cluster-loopback:
	@$(PYTHON) tools/check-cluster-loopback.py

# Exercise the actual vendored lwIP heap and pbuf code under host threads. The
# host ports supply architecture types, CPU identity and lock shims; allocator and
# reference-count algorithms come from the kernel's lwIP sources.
$(TMPDIR)/a20-host-test_lwip_allocator_concurrency \
$(TMPDIR)/a20-host-test_lwip_allocator_single_lane: \
        tools/tests/test_lwip_allocator_concurrency.c \
        kernel/external/lwip/src/core/mem.c \
        kernel/external/lwip/src/core/memp.c \
        kernel/external/lwip/src/core/pbuf.c \
        kernel/net/lwip_compat.c \
        kernel/net/lwip_port/arch/heap.h \
        tools/tests/lwip_host_port/core/lock.h \
        tools/tests/lwip_host_port/arch/cc.h \
        tools/tests/lwip_host_port/lwipopts.h \
        tools/tests/net_lane_host_port/core/cpu.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -ffunction-sections -fdata-sections \
	  -DCONFIG_X86_64 -DCONFIG_NR_CPUS=9 \
	  -DCONFIG_NET_LANES=$(if $(findstring single_lane,$@),1,4) \
	  -Itools/tests/lwip_host_port \
	  -Itools/tests/net_lane_host_port \
	  -Ikernel/external/lwip/src/include -Ikernel/include -Ikernel \
	  $(filter %.c,$^) -Wl,--gc-sections -o $@

# Compile the real lane queue implementation, with only CPU identity shimmed.
$(TMPDIR)/a20-host-test_net_lane_concurrency: tools/tests/test_net_lane_concurrency.c \
        kernel/net/net_lane.c kernel/include/net/net_lane.h kernel/net/net_profile.h
	$(HOST_CC) $(HOST_CFLAGS) -pthread -DCONFIG_NET_LANES=4 -DCONFIG_NR_CPUS=4 \
	  -Itools/tests/net_lane_host_port -Ikernel/include -Ikernel \
	  tools/tests/test_net_lane_concurrency.c kernel/net/net_lane.c -o $@

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

# Prove the task-group leader remains alive while linked members can still
# update its embedded tg_next chain, using the production link/unlink bodies.
check-tg-leader-lifetime:
	$(PYTHON) tools/tests/test_tg_leader_lifetime.py

# Minimal ISO9660 test image for the isofs driver (no mkisofs/xorriso needed).
$(ISOFS_IMG): tools/mkisofs_test.c
	@mkdir -p $(BUILD_DIR) $(TMPDIR)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $(TMPDIR)/a20-mkisofs
	$(TMPDIR)/a20-mkisofs $@

check-vfs-abstraction: smoke-vfs-stress
	@$(PYTHON) tools/gates.py vfs-abstraction

check-abi-boundary:
	@$(PYTHON) tools/gen_linux_syscall_coverage.py --check
	@$(PYTHON) tools/gates.py abi-boundary

# The other half of the dual-ABI blind spot: the Linux side has had two coverage
# generators, so a new LINUX_SYSCALL needs a coverage row and an envelope class,
# while a new A20_NATIVE_SYSCALL needed neither.  check-native-abi-coverage
# compares the three Native sources against each other (registration table,
# syscall_nr.h, docs/native-abi/) and names the entries that disagree;
# check-abi-config-guard forbids the bare `#ifdef CONFIG_ABI_*` spelling that
# silently drops one ABI's code from a build.  Both are host-side.
check-native-abi-coverage:
	@$(PYTHON) tools/gates.py native-abi-coverage

check-abi-config-guard:
	@$(PYTHON) tools/gates.py abi-config-guard

check-driver-core-model: smoke-driver-lifecycle
	@$(PYTHON) tools/gates.py driver-core-model

check-drm-store-locking:
	@$(PYTHON) tools/gates.py drm-store-locking

check-external-dependency-boundary:
	@$(PYTHON) tools/gates.py external-dependency-boundary

# ----------------------------------------------------------------
# `make check` -- the fast host-side gate tier
# ----------------------------------------------------------------
# Every target CI's `toolchain-gates` job runs, in the order the job runs them.
# Keeping the list in one variable (rather than as bare prerequisites) is what
# lets the recipe below report per-gate results and let `make help` print the
# same list; a second hand-typed copy is exactly how a gate stops being run
# locally while still passing in CI.
#
# The list is a *tier*, not a wish list: every member is host-side (pure Python,
# rg, or the host gcc) and needs neither a cross toolchain nor QEMU, so `make
# check` is always runnable on a bare checkout.  The 11 remaining members of
# check-doc-test-gates each boot a QEMU guest and belong to the `smoke` job; the
# two Native ABI gates below are its other host-side members, so they are
# counted in the tier rather than in that 11.
#
# When this list changes, .github/workflows/ci.yml's toolchain-gates job must
# change with it; that file is the other half of the contract and is not
# edited from here.
CHECK_FAST_GATES := \
    check-manifests \
    check-a20-tests \
    check-honesty-policy \
    check-smoke-cases \
    check-xlator-guests \
    host-tests \
    check-cluster-unit \
    check-cluster-fuzz \
    check-cluster-wakeup \
    check-slab-cpu-window \
    check-virtio-net-completion \
    check-drm-abi \
    check-task-lifetime-boundary \
    check-smp-platform-boundary \
    check-io-progress-model \
    check-external-dependency-boundary \
    check-abi-boundary \
    check-native-abi-coverage \
    check-abi-config-guard \
    check-envelope-coverage \
    check-task-state-boundary \
    check-abi-smoke-gate \
    test-nat-rewrite \
    test-signal-core \
    check-doc-drift

# Recurse per gate instead of listing them as prerequisites.  A single make
# would run them in parallel under -j and stop at the first failure; one
# $(MAKE) per gate keeps the jobserver honest (so `make -j check` does not
# oversubscribe), keeps the order deterministic, and lets every gate run so a
# single `make check` reports *all* the drift in the tree rather than the first
# symptom of it.  That is the same "fail-closed, name the broken assertion"
# stance the individual gates take.
#
# `check-format` is deliberately NOT a member: it needs a clang-format that the
# CI runner does not install, so folding it in here would make `make check`
# mean something CI cannot reproduce.  Run it explicitly.
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
.PHONY: test-nat-rewrite
test-nat-rewrite:
	@sh tools/test-nat-rewrite-host.sh $(NETFILTER_REWRITE_IMPL)
	@echo "test-nat-rewrite ($(NETFILTER_REWRITE_IMPL)): PASS"

# ----------------------------------------------------------------
# Signal decision-core host tests
# ----------------------------------------------------------------
# The signal core is pure decision logic, so its Rust unit/oracle vectors and
# the C<->Rust differential sweep run on the host in about a second and need no
# target runtime. Also re-runs the frozen Python/C oracle models.
.PHONY: test-signal-core
test-signal-core:
	@sh tools/test-signal-core-host.sh
	@echo "test-signal-core: PASS"

# ----------------------------------------------------------------
# clang-format drift gate (docs/CONTRIBUTING.md 4.2)
# ----------------------------------------------------------------
# The one gate whose tool may legitimately be absent, so it is the one gate that
# must not hard-fail on a missing binary.  A contributor without clang-format
# gets a SKIP line naming the package, not a red build they cannot fix and not
# a silent pass they cannot see.  Override with CLANG_FORMAT=/path/to/binary.
CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null)

.PHONY: check-format format-baseline
check-format:
ifeq ($(CLANG_FORMAT),)
	@echo "check-format: SKIP -- clang-format is not installed, so no formatting" \
	  "assertion ran.  Install it (apt-get install clang-format) or set" \
	  "CLANG_FORMAT=/path/to/clang-format.  This gate reports drift; it never" \
	  "rewrites a source file."
else
	@$(PYTHON) tools/clang_format_gate.py --clang-format "$(CLANG_FORMAT)"
endif

# Re-record the ratchet.  Deliberately a separate target from check-format so
# that widening the exemption set is always an explicit, reviewable act: the
# diff of tools/clang-format-baseline.txt *is* the record of what was accepted.
format-baseline:
ifeq ($(CLANG_FORMAT),)
	@echo "format-baseline: SKIP -- clang-format is not installed (see check-format)"
else
	@$(PYTHON) tools/clang_format_gate.py --clang-format "$(CLANG_FORMAT)" --write
endif

check-cluster-reliable:
	$(PYTHON) tools/check-cluster-udp.py --faults --restart --log-dir .kernel-build/cluster/reliable

.PHONY: check-cluster-send check-cluster-smp
check-cluster-smp:
	NR_CPUS=2 $(PYTHON) tools/check-cluster-udp.py --send --faults --random-loss 0.1 --profile 3 --timeout 240 --log-dir .kernel-build/cluster/send-smp2

check-cluster-send:
	$(PYTHON) tools/check-cluster-udp.py --send --faults --random-loss 0.1 --profile 3 --timeout 240 --log-dir .kernel-build/cluster/send-server
	$(PYTHON) tools/check-cluster-udp.py --send --routed --faults --profile 3 --peer-profile 2 --timeout 180 --log-dir .kernel-build/cluster/send-routed-mixed
	$(PYTHON) tools/check-cluster-gateway.py --send --timeout 180

.PHONY: check-cluster-mesh
check-cluster-mesh:
	$(PYTHON) tools/check-cluster-mesh.py

.PHONY: check-cluster-fuzz
check-cluster-fuzz:
	$(PYTHON) tools/check-cluster-fuzz.py

.PHONY: check-cluster-fuzz-smoke check-cluster-performance
check-cluster-fuzz-smoke:
	$(PYTHON) tools/check-cluster-fuzz.py --seconds 300

check-cluster-performance:
	$(PYTHON) tools/check-cluster-performance.py

.PHONY: check-cluster-load
check-cluster-load:
	$(PYTHON) tools/check-cluster-load.py

.PHONY: check-virtio-net-completion
check-virtio-net-completion:
	$(PYTHON) tools/check-virtio-net-completion.py

.PHONY: check-cluster-wakeup
check-cluster-wakeup:
	$(PYTHON) tools/check-cluster-wakeup.py

.PHONY: check-slab-cpu-window
check-slab-cpu-window:
	$(PYTHON) tools/check-slab-cpu-window.py

.PHONY: check-cluster-uart
check-cluster-uart:
	$(PYTHON) tools/check-cluster-uart.py

.PHONY: check-cluster-gateway
check-cluster-gateway:
	$(PYTHON) tools/check-cluster-gateway.py

.PHONY: demo-wordcount
demo-wordcount:
	$(PYTHON) tools/check-cluster-wordcount.py
	$(PYTHON) tools/check-cluster-wordcount.py --mixed

.PHONY: check-cluster-hardening
check-cluster-hardening:
	$(PYTHON) tools/check-cluster-udp.py --faults --restart --stress --random-loss 0.1 --corrupt --timeout 420 --log-dir .kernel-build/cluster/random-loss1000
	$(PYTHON) tools/check-cluster-udp.py --faults --restart --random-delay --corrupt --hello-flood --timeout 240 --log-dir .kernel-build/cluster/delay-crc-flood
	$(PYTHON) tools/check-cluster-udp.py --faults --partition --slow-consumer --timeout 150 --log-dir .kernel-build/cluster/partition-slow

.PHONY: check-cluster-instance
check-cluster-instance:
	$(PYTHON) tools/check-cluster-instance.py

.PHONY: check-cluster-objects
check-cluster-objects:
	$(PYTHON) tools/check-cluster-objects.py

.PHONY: check-object-directory-portability
check-object-directory-portability:
	$(PYTHON) tools/check-object-directory-portability.py

.PHONY: check-cluster-apps
check-cluster-apps:
	$(PYTHON) tools/check-cluster-apps.py
