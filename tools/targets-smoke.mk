# Most smoke-* targets here are thin wrappers over `tools/a20 test
# instances/*.toml` or `tools/smoke.py <case>`; both of those gate host
# resources through tools/a20_resource.py before booting QEMU, so a full host
# never turns into an OOM-kill lottery.  (A20_WAIT_TIMEOUT defaults to waiting
# forever there; CI passes a bounded value.)  tools/stm32.mk's
# run-stm32f103-qemu is the deliberate exception: the stm32vldiscovery machine
# has fixed 20 KiB of SRAM and no -m/-smp to read, so there is nothing for a
# host-side gate to protect.

# Thin wrapper: the smoke definition lives in instances/smoke-riscv64.toml.
smoke-riscv64:
	tools/a20 test smoke-riscv64

check-a20-idl: user/svc/a20_services_idl.h
	@$(PYTHON) tools/gensync.py a20-idl \
		--idl user/svc/a20_services.idl \
		--committed user/svc/a20_services_idl.h \
		--out-name a20_services_idl.h \
		-- $(A20_IDL_PYTHON) tools/a20idl.py user/svc/a20_services.idl \
		@TMP@/a20_services_idl.h

# Thin wrapper: the smoke definition lives in instances/smoke-iommu-discovery.toml.
smoke-iommu-discovery:
	tools/a20 test smoke-iommu-discovery

smoke-iommu-udriver-isolation:
	$(PYTHON) tools/smoke.py smoke-iommu-udriver-isolation

# Thin wrapper: the smoke definition lives in instances/smoke-loongarch64.toml.
smoke-loongarch64:
	tools/a20 test smoke-loongarch64

# Thin wrapper: the smoke definition lives in instances/smoke-aarch64.toml.
smoke-aarch64:
	tools/a20 test smoke-aarch64

# Thin wrapper: the smoke definition lives in instances/smoke-x86_64.toml.
smoke-x86_64:
	tools/a20 test smoke-x86_64

# Behavioral SMP gate: boot a NR_CPUS=2 BRINGUP kernel and require it to
# complete bring-up and power off, exercising SMP init on real secondaries.
smoke-smp-bringup:
	$(PYTHON) tools/smoke.py smoke-smp-bringup

# Thin wrapper: the smoke definition lives in instances/smoke-arm32.toml.
smoke-arm32:
	tools/a20 test smoke-arm32

# Thin wrapper: the smoke definition lives in instances/smoke-riscv32.toml.
smoke-riscv32:
	tools/a20 test smoke-riscv32

smoke-arch-mmu-matrix:
	@$(PYTHON) tools/smoke.py arch-mmu-matrix \
		--input-delay "$(SMOKE_INPUT_DELAY)" \
		--timeout "$(SMOKE_TIMEOUT)"
# Thin wrapper: the smoke definition lives in instances/smoke-ppc64le.toml.
smoke-ppc64le:
	tools/a20 test smoke-ppc64le

# Thin wrapper: the smoke definition lives in instances/smoke-abi-linux.toml.
smoke-abi-linux:
	tools/a20 test smoke-abi-linux

smoke-net-iface:
	tools/a20 test smoke-net-iface

smoke-envelope:
	$(PYTHON) tools/smoke.py smoke-envelope

smoke-envelope-pilot:
	$(PYTHON) tools/smoke.py smoke-envelope-pilot

smoke-envelope-bench:
	$(PYTHON) tools/smoke.py smoke-envelope-bench

smoke-envelope-corpus:
	$(PYTHON) tools/smoke.py smoke-envelope-corpus

smoke-a20-channel:
	$(PYTHON) tools/smoke.py smoke-a20-channel

smoke-ptrace:
	$(PYTHON) tools/smoke.py smoke-ptrace

smoke-netfilter:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/netfilter-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'netfilter_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NETFILTER_TEST: PASS' "$$log"; then \
		echo "smoke-netfilter: PASS; log saved to $$log"; \
	elif grep -q 'NETFILTER_TEST: ABSENT' "$$log"; then \
		echo "smoke-netfilter: FAIL -- netfilter transmit hook absent (netfilter_output()"; \
		echo "  is not called from a20_lwip_linkoutput()). Evidence:"; \
		grep -E 'NETFILTER_TEST' "$$log" | tail -n 5; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif grep -q 'NETFILTER_TEST: SKIP' "$$log"; then \
		echo "smoke-netfilter: FAIL -- netfilter data plane NOT proven. The control"; \
		echo "  surface worked but out_dropped never moved. This used to exit 0 and"; \
		echo "  report SKIP as a pass, which manufactured confidence in a filter"; \
		echo "  whose data path may not exist at all. A SKIP is not proof, so it"; \
		echo "  cannot be green. Evidence:"; \
		grep -E 'NETFILTER_TEST|out_packets|out_dropped' "$$log" | tail -n 20; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-netfilter: timeout without verdict; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-netfilter: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-network-suite:
	$(PYTHON) tools/smoke.py smoke-network-suite

# netctl only inspects local kernel state; it never accepts an inbound
# connection, so it must not depend on a free host port 5555.  That is now
# baked into the case argv in tools/smoke_cases.py (no hostfwd token), so the
# old `NET_HOSTFWD=` target-specific override has nothing left to do.
smoke-netctl:
	$(PYTHON) tools/smoke.py smoke-netctl
smoke-network-suite-aarch64:
	$(PYTHON) tools/smoke.py smoke-network-suite-aarch64

smoke-proc-a20:
	$(PYTHON) tools/smoke.py smoke-proc-a20

smoke-proc-stress:
	$(PYTHON) tools/smoke.py smoke-proc-stress

smoke-procfs-stress:
	$(PYTHON) tools/smoke.py smoke-procfs-stress

smoke-mm-stress:
	$(PYTHON) tools/smoke.py smoke-mm-stress

# Regression gate for the V8/Node.js hint-fallback fix: mmap with a
# hint above USER_VA_LIMIT must fall back, not fail with ENOMEM.
smoke-mmprobe:
	$(PYTHON) tools/smoke.py smoke-mmprobe

smoke-oom-stress:
	$(PYTHON) tools/smoke.py smoke-oom-stress

# Swap runtime gate: swap_test binds a ramfs-backed loop device, then runs
# mkswap -> swapon -> duplicate-swapon(EBUSY) -> swapoff, asserting
# /proc/swaps and sysinfo totalswap transitions along the way.
smoke-swap:
	$(PYTHON) tools/smoke.py smoke-swap

smoke-mm-fork-exec-race:
	$(PYTHON) tools/smoke.py smoke-mm-fork-exec-race

smoke-vfs-stress:
	$(PYTHON) tools/smoke.py smoke-vfs-stress

smoke-fsync-durability:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	$(MAKE) -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/fsync-durability-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'fsync_durability_test' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
			> "$$log" 2>&1 || status=$$?; \
	if grep -q 'FSYNC_TEST: PASS' "$$log"; then \
		echo "smoke-fsync-durability: PASS; log saved to $$log"; \
	else \
		echo "smoke-fsync-durability: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-sysv-ipc-abi:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	$(MAKE) -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/sysv-ipc-abi-riscv64.log"; \
	status=0; \
	$(TIMEOUT) --expect '# ' \
		--send-line 'sysv_ipc_abi_test' --send-line 'poweroff' \
		$(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1 \
		-device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'SYSV_IPC_ABI: PASS' "$$log"; then \
		echo "smoke-sysv-ipc-abi: PASS; log saved to $$log"; \
	else \
		echo "smoke-sysv-ipc-abi: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

smoke-vfs-edge:
	$(PYTHON) tools/smoke.py smoke-vfs-edge

smoke-io-event:
	$(PYTHON) tools/smoke.py smoke-io-event

smoke-syscall-ext:
	$(PYTHON) tools/smoke.py smoke-syscall-ext

smoke-sched-stress:
	$(PYTHON) tools/smoke.py smoke-sched-stress

smoke-futex-stress:
	$(PYTHON) tools/smoke.py smoke-futex-stress

smoke-futex-stress-aarch64:
	$(PYTHON) tools/smoke.py smoke-futex-stress-aarch64

smoke-scm-stress:
	$(PYTHON) tools/smoke.py smoke-scm-stress

smoke-evdev-stress:
	$(PYTHON) tools/smoke.py smoke-evdev-stress

smoke-signalfd-stress:
	$(PYTHON) tools/smoke.py smoke-signalfd-stress

smoke-pty-stress:
	$(PYTHON) tools/smoke.py smoke-pty-stress

smoke-timeout-test:
	$(PYTHON) tools/smoke.py smoke-timeout-test

# ================================================================
# Coredump smoke (fatal-signal ELF core dump generation)
# ================================================================
smoke-coredump:
	$(PYTHON) tools/smoke.py smoke-coredump

# ================================================================
# Poll/timer edge-semantics smokes (Linux ABI poll + timer areas)
# ================================================================
smoke-poll-edge:
	$(PYTHON) tools/smoke.py smoke-poll-edge

smoke-timer-edge:
	$(PYTHON) tools/smoke.py smoke-timer-edge

# ================================================================
# Mount namespace smoke (unshare/setns CLONE_NEWNS + /proc/<pid>/ns/mnt)
# ================================================================
smoke-mntns:
	$(PYTHON) tools/smoke.py smoke-mntns

# ================================================================
# PCI bridge traversal smoke
# ================================================================
# Boots q35 with a virtio-blk device hung off two chained pcie-root-ports,
# so it lives on a bus *behind* a header-type-1 bridge rather than on the
# root bus.  The kernel must walk secondary/subordinate buses to see it.
#
# Honest scope: this is a REGRESSION GUARD, not proof the traversal works.
# The x86_64 board calls pci_enumerate(PCI_ECAM_BASE, 0, 255), so the old
# flat [bus_start, bus_end) scan happened to cover these buses too and this
# gate would have passed before the fix.  The traversal actually changes
# behaviour on boards that report a narrow range -- qemu-virt-riscv64 passes
# (0, 1) and virtualbox-aarch64 passes firmware-allocated ranges -- and this
# QEMU build cannot exercise either, so the coverage gap is deliberate and
# recorded in docs/server-readiness.md rather than papered over.
smoke-pci-bridge: NET_HOSTFWD=
smoke-pci-bridge:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/pci-bridge-x86_64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=xb \
		-device pcie-root-port,id=rp1,bus=pcie.0,addr=0x4,chassis=1 \
		-device pcie-root-port,id=rp2,bus=rp1,addr=0x0,chassis=2 \
		-device virtio-blk-pci,drive=xb,bus=rp2,addr=0x0 \
		-kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'bridges walked' "$$log" && \
	   grep -qE '\[BUS\] pci 0[12]:00\.0 id=1b36:000c .*class=06:04:00' "$$log" && \
	   grep -qE '\[BUS\] pci 02:00\.0 id=1af4:1042' "$$log" && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-pci-bridge: PASS (device behind 2 nested root ports enumerated); log saved to $$log"; \
	else \
		echo "smoke-pci-bridge: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# lwIP mempool pressure smoke
# ================================================================
# /proc/a20/netmem surfaces lwIP's per-pool used/max/err counters.  MEMP_STATS
# derives to 1 here (MEMP_MEM_MALLOC is 0), so lwIP already maintains those
# counters, but the only reader it ships is compiled out by LWIP_STATS_DISPLAY=0
# -- this file is the only way to read them.
#
# Scope: the err==0 assertion is real, and it is the signal that would catch a
# pool sized too small for a given workload.  It does NOT justify the current
# pool size: peak max on a loopback-backed suite is tiny, so this gate says
# nothing about high-BDP sizing.  See docs/server-readiness.md.
smoke-lwip-memp: NET_HOSTFWD=
smoke-lwip-memp:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/lwip-memp-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'network_suite\ncat /proc/a20/netmem\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'NETWORK_SUITE: PASS' "$$log" && \
	   grep -qE '^PBUF_POOL +[0-9]+ ' "$$log" && \
	   grep -qE '^TCP_SEG +[0-9]+ ' "$$log" && \
	   grep -qE '^TCP_PCB_LISTEN +[0-9]+ ' "$$log" && \
	   ! grep -qE '^(PBUF_POOL|PBUF|TCP_SEG|TCP_PCB|TCP_PCB_LISTEN|UDP_PCB) +[0-9]+ +[0-9]+ +[0-9]+ +[1-9]' "$$log"; then \
		echo "smoke-lwip-memp: PASS (6 pools exposed, err=0 after network suite); log saved to $$log"; \
	else \
		echo "smoke-lwip-memp: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# SMP cross-core lock contention smoke
# ================================================================
# net_stress_test puts WORKERS concurrent TCP transfers through the stack, and
# this gate runs it on a real NR_CPUS=4 build so cross-core spinlock contention
# is actually observable.
#
# Why the explicit NR_CPUS: Makefile defaults NR_CPUS ?= 1, so every default
# dev/smoke gate is single-core.  Passing -smp to QEMU does not help, because
# the kernel only brings up NR_CPUS CPUs -- and one CPU can never contend a
# spinlock.  That is how "lwip: 0 0" reads like a healthy result when nothing
# was measured at all.
#
# What this gate deliberately does NOT assert: any contention number.  Acquire
# and spin counts depend on scheduling and are not reproducible run to run, so
# a threshold here would be a flaky gate and would invite tuning toward a
# magic number.  It asserts the deterministic parts only -- the stress test
# transfers correctly, the counters render, and the per-site spin column is
# real spin data rather than a copy of the acquire column.
#
# That last check has an honest limit.  Call-site attribution hashes into a
# fixed 32-slot table (LOCK_CALLSITE_SAMPLES) and a collision drops that
# caller's spin count while the lock total still counts it, so at low acquire
# counts a run can legitimately attribute nothing.  The assertion is therefore
# conditional: with nothing attributed there is nothing to check, and it only
# bites when attribution did happen.  It is not a guarantee that every spin is
# attributed, and an earlier unconditional form of it was flaky for exactly
# that reason.
smoke-smp-lock-contention: NET_HOSTFWD=
smoke-smp-lock-contention:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/smp-lock-contention-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'cat /proc/a20/lock_contention\ncat /proc/a20/perf\nnet_stress_test\ncat /proc/a20/perf\ncat /proc/a20/lock_contention\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 4 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	proc_split=$$(awk '/^proc: /{b++; if(b==1){ba=$$2;bs=$$3} if(b==2){print "boot "ba" acq / "bs" spins | stress-only "($$2-ba)" acq / "($$3-bs)" spins"}} END{if(b<2)print "UNAVAILABLE (only "b" lock_contention block"b"; tail console command was dropped)"}' "$$log"); \
	lwip_total=$$(awk '/^lwip: /{b++; if(b==2){t=$$3}} END{print t+0}' "$$log"); \
	lwip_stress_spins=$$(awk '/^lwip: /{b++; if(b==1){bs=$$3} if(b==2){d=$$3-bs}} END{print d+0}' "$$log"); \
	proc_max=$$(awk '/^proc: /{b++; if(b==2){v=$$4; sub(/^max=/,"",v); print v+0; exit}}' "$$log"); \
	site_acq=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2)a+=$$3} END{print a+0}' "$$log"); \
	site_spin=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2)s+=$$4} END{print s+0}' "$$log"); \
	site_max=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2){v=$$5; sub(/^max=/,"",v); if (v+0>m) m=v+0}} END{print m+0}' "$$log"); \
	tlb_enters=$$(awk '/^mm_context_enters:/{e=$$2} END{print e+0}' "$$log"); \
	tlb_waits=$$(awk '/^mm_tlb_converge_waits:/{w=$$2} END{print w+0}' "$$log"); \
	tlb_flushes=$$(awk '/^mm_tlb_converge_flushes:/{f=$$2} END{print f+0}' "$$log"); \
	if grep -q 'NET_STRESS_TEST: PASS' "$$log" && \
	   grep -qE '^lwip: [0-9]+ [0-9]+ max=[0-9]+$$' "$$log" && \
	   grep -qE '^proc: [0-9]+ [0-9]+ max=[0-9]+$$' "$$log" && \
	   { [ "$$lwip_total" -eq 0 ] || [ "$$site_spin" -ge $$((lwip_total * 9 / 10)) ]; } && \
	   [ "$$tlb_enters" -gt 0 ] && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-smp-lock-contention: PASS (4-core run; stress ok, counters render, attribution accounts for $$site_spin of $$lwip_total lock-level spins across $$site_acq sampled acquires); log saved to $$log"; \
		echo "lwip stress-window: $$lwip_stress_spins spins over the run. Recorded, NOT asserted:" \
		     "arch_cpu_relax() iterations have no fixed conversion to wall time under TCG, and repeated" \
		     "4-core runs of this workload have spanned 0 to ~920000, so any magnitude threshold here" \
		     "would be flaky. The structural reductions are asserted in the kernel build instead (see the" \
		     "net_socket_t and TCP_MSG size _Static_asserts); this number is here so drift stays visible."; \
		grep -E '^(lwip|proc|runq): ' "$$log" || true; \
		echo "worst single acquire (cumulative): proc_lock $$proc_max spins, lwip site $$site_max spins"; \
		if [ "$$lwip_total" -eq 0 ]; then \
			echo "note: no lwip contention in this window, so the attribution invariant was satisfied vacuously rather than exercised"; \
		fi; \
		echo "proc_lock windows -- $$proc_split"; \
		echo "TLB convergence inside proc_lock: $$tlb_enters enters, $$tlb_waits flushed at least once, $$tlb_flushes local ASID flushes"; \
	else \
		echo "smoke-smp-lock-contention: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi
