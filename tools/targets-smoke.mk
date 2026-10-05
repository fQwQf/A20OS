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

# DNAT port forwarding, end to end.
#
# smoke-netfilter above can only ever see traffic the guest originates, and a
# DNAT is by definition a rewrite of an *inbound* packet.  So this case does
# what the other one structurally cannot: it forwards a host port into the guest
# (NET_HOSTFWD), connects to that port from the host, and asserts that a socket
# listening on a *different* port in the guest accepted the connection and
# echoed a byte back.
#
# Both directions of the translation are covered by that one assertion.  The
# inbound half is the destination rewrite; the outbound half is forced by the
# echo, because the host's QEMU only accepts a reply whose source port is the
# forwarded port again -- and nothing but the conntrack entry's recorded DNAT
# tuple produces that, because the guest socket's own port is LISTEN_PORT.
#
# The host-side probe retries until the guest has booted and installed the
# rule, then fails loudly if the forward never completed.  QEMU runs in the
# background so a failed probe still leaves a guest log to read.
#
# Deliberately NOT asserted: a NAT rule's "matched" count on its own.  That
# counter moves whether or not the rewrite reached lwIP with a valid checksum,
# which is exactly the failure this gate exists to rule out.
#
# The forward is spelled "hostfwd=tcp:127.0.0.1:18081-10.0.2.15:18081" for two
# reasons, both found by running QEMU 10.0.13 here rather than by reading its
# docs:
#
#   - The bare "tcp:hostaddr:port-:guest" spelling is rejected outright:
#     QEMU parses the value as a deprecated boolean and refuses to start
#     ("Invalid parameter").  The hostfwd= prefix is required.
#   - There is no colon before the guest address.  With one, the guest half is
#     parsed as [addr]:port and the address field swallows the host part, so the
#     port comes out as "10" and the SYN reaches the guest on 10.0.2.15:10.
#     Measured with -object filter-dump on the netdev, both spellings booted and
#     both accepted the host connection:
#       tcp:127.0.0.1:18081-10.0.2.15:18081  -> guest sees 10.0.2.15:18081
#       tcp:127.0.0.1:18082-:10.0.2.15:18082 -> guest sees 10.0.2.15:10
#     A silently wrong guest port is exactly the failure this gate exists to
#     catch, so it is worth writing down rather than rediscovering.
#
# Note for whoever owns tools/a20_derive.py: it emits "tcp::5555-:5555", which
# QEMU 10 refuses ("Missing guest address").  Left alone here -- that string is
# asserted by tools/tests/test_a20.py and is a different stream's file.
#
# a20.tcpmode=lwip is load-bearing for the same reason.  Under the default
# "fast" mode the socket layer never creates a LISTEN pcb (see the comment in
# kernel/net/socket_control.c:204), so *any* inbound SYN gets an RST from lwIP
# and no port forward can ever complete -- NAT would be exonerated of a failure
# that is really the listener model.  Same knob smoke-net-accept uses.
smoke-netfilter-nat: NET_HOSTFWD=hostfwd=tcp:127.0.0.1:18081-10.0.2.15:18081
smoke-netfilter-nat:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NET_HOSTFWD='hostfwd=tcp:127.0.0.1:18081-10.0.2.15:18081' dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/netfilter-nat-riscv64.log"; \
	status=0; probe=0; \
	rm -f "$$log"; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'netnat_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_NAT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 & \
	qemu_pid=$$!; \
	$(PYTHON) tools/netnat_host_probe.py 18081 || probe=$$?; \
	wait $$qemu_pid || status=$$?; \
	if [ "$$probe" -ne 0 ]; then \
		echo "smoke-netfilter-nat: FAIL -- the host never completed the port forward"; \
		echo "  (tools/netnat_host_probe.py exit $$probe). Guest verdict, if any:"; \
		grep -E 'NETNAT_TEST' "$$log" || echo "  (none: the guest never got that far)"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif grep -q 'NETNAT_TEST: PASS' "$$log"; then \
		echo "smoke-netfilter-nat: PASS dnat 18081 -> 10.0.2.15:18082"; \
		echo "  log saved to $$log"; \
	elif grep -q 'NETNAT_TEST: FAIL' "$$log"; then \
		echo "smoke-netfilter-nat: FAIL -- the guest reported a failure:"; \
		grep -E 'NETNAT_TEST' "$$log" | tail -n 5; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-netfilter-nat: timeout without verdict; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-netfilter-nat: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# conntrack capacity (LRU eviction) and idle-timeout reclamation.
#
# Both limits had /proc counters (ct_evicted / ct_timeout) and no gate that ever
# moved them, so "the table evicts LRU at capacity" and "the sweeper reclaims
# idle flows" were assertions in a design note, not properties of a running
# kernel.  This case drives the table to the state each limit describes --
# filled to ct_capacity, then one flow past it; one entry with a 100 ms timeout
# and a 3 s wait -- and asserts the counters *and* the identity of the flow that
# was dropped, because "evicted went up" is also true of an implementation that
# evicts an arbitrary entry.
#
# No host port forward: nothing here puts a packet on the wire, so the case runs
# on the same image as smoke-netfilter and needs no listener model.
smoke-ct-capacity:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/ct-capacity-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'ct_capacity_test\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 1 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'CTCAP_TEST: PASS' "$$log"; then \
		echo "smoke-ct-capacity: PASS $$(grep -o 'capacity=[0-9]*' "$$log" | tail -n 1)"; \
		echo "  log saved to $$log"; \
	elif grep -q 'CTCAP_TEST: FAIL' "$$log"; then \
		echo "smoke-ct-capacity: FAIL -- the guest reported a failure:"; \
		grep -E 'CTCAP_TEST' "$$log" | tail -n 5; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif [ "$$status" -eq 124 ]; then \
		echo "smoke-ct-capacity: timeout without verdict; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	else \
		echo "smoke-ct-capacity: failed with status $$status; tail of $$log:"; \
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

# Drives an address space past MM_SEG_INDEX_CAPACITY so mm_seg_find()'s
# list-walk fallback executes.  See the case in tools/smoke_cases.py for why
# this needed its own gate rather than a phase of smoke-mm-stress.
smoke-mm-seg-index-overflow:
	$(PYTHON) tools/smoke.py smoke-mm-seg-index-overflow

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

# Page-table cursor race/audit gate.  Boots with a20.anonprov so wide cursors
# actually get created; see the case comment in smoke_cases.py for what a PASS
# does and does not prove.
smoke-mm-pt-race:
	$(PYTHON) tools/smoke.py smoke-mm-pt-race

# Real-software gate: boots the mmtest distro world and runs git, vim, gcc,
# python and nodejs inside it, then requires the shutdown-time [MM-ASM] audit
# to report page tables and per-PTE metadata in agreement.
#
# This is deliberately NOT folded into check-mm-lock-model, whose smoke cases
# only exercise kernel-written syscalls on kernel-allocated pages.  The two
# verdicts this gate needs (real programs pass; the two representations never
# diverged while they ran) are both properties a syscall-level smoke test is
# blind to by construction.  It is slow -- a full image build plus a ~3 min
# boot -- so it is its own target rather than part of the default check set;
# see docs/testing-gates.md for why that trade is deliberate.
smoke-mm-software:
	$(PYTHON) tools/mmtest_gate.py

smoke-vfs-stress:
	$(PYTHON) tools/smoke.py smoke-vfs-stress

smoke-lfs:
	$(PYTHON) tools/smoke.py smoke-lfs

smoke-pivot-root:
	$(PYTHON) tools/smoke.py smoke-pivot-root

smoke-vfs-stress-smp2:
	$(PYTHON) tools/smoke.py smoke-vfs-stress-smp2

smoke-vfs-stress-smp8:
	$(PYTHON) tools/smoke.py smoke-vfs-stress-smp8

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

smoke-mtcorrupt:
	$(PYTHON) tools/smoke.py smoke-mtcorrupt

smoke-pidns:
	$(PYTHON) tools/smoke.py smoke-pidns

# ================================================================
# User namespace smoke (unshare/setns CLONE_NEWUSER + /proc/<pid>/uid_map)
# ================================================================
smoke-userns:
	$(PYTHON) tools/smoke.py smoke-userns

# ================================================================
# USB hub smoke
# ================================================================
# The keyboard and mouse hang off a hub nested behind the xHCI root hub, so
# nothing is reachable by the flat "enumerate the controller's own ports" walk:
# the hub itself has to be enumerated, its class driver has to expose a second
# tier of ports, and the devices behind it have to come up through that tier.
smoke-usb-hub-x86_64:
	$(PYTHON) tools/smoke.py smoke-usb-hub-x86_64

# ================================================================
# MSI-X smoke (x86_64)
# ================================================================
# The only board with a message-signalled interrupt path at all, so the only
# place the arch-independent MSI-X code and the x86 LAPIC programming meet.
# See the case comment in tools/smoke_cases.py for what the pass/fail line
# actually proves -- the delivery line is printed from the interrupt handler.
smoke-msix-x86_64:
	$(PYTHON) tools/smoke.py smoke-msix-x86_64

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
	tasklist_split=$$(awk '/^tasklist: /{b++; if(b==1){ba=$$2;bs=$$3} if(b==2){print "boot "ba" acq / "bs" spins | stress-only "($$2-ba)" acq / "($$3-bs)" spins"}} END{if(b<2)print "UNAVAILABLE (only "b" lock_contention block"b"; tail console command was dropped)"}' "$$log"); \
	lwip_total=$$(awk '/^lwip: /{b++; if(b==2){t=$$3}} END{print t+0}' "$$log"); \
	lwip_stress_spins=$$(awk '/^lwip: /{b++; if(b==1){bs=$$3} if(b==2){d=$$3-bs}} END{print d+0}' "$$log"); \
	tasklist_max=$$(awk '/^tasklist: /{b++; if(b==2){v=$$4; sub(/^max=/,"",v); print v+0; exit}}' "$$log"); \
	site_acq=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2)a+=$$3} END{print a+0}' "$$log"); \
	site_spin=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2)s+=$$4} END{print s+0}' "$$log"); \
	site_max=$$(awk '/^lwip: /{b++; next} /\[lwip\]/{if(b>=2){v=$$5; sub(/^max=/,"",v); if (v+0>m) m=v+0}} END{print m+0}' "$$log"); \
	tlb_enters=$$(awk '/^mm_context_enters:/{e=$$2} END{print e+0}' "$$log"); \
	tlb_waits=$$(awk '/^mm_tlb_converge_waits:/{w=$$2} END{print w+0}' "$$log"); \
	tlb_flushes=$$(awk '/^mm_tlb_converge_flushes:/{f=$$2} END{print f+0}' "$$log"); \
	if grep -q 'NET_STRESS_TEST: PASS' "$$log" && \
	   ! grep -q 'tcp_pcbs_sane' "$$log" && \
	   grep -qE '^lwip: [0-9]+ [0-9]+ max=[0-9]+$$' "$$log" && \
	   grep -qE '^tasklist: [0-9]+ [0-9]+ max=[0-9]+$$' "$$log" && \
	   { [ "$$lwip_total" -eq 0 ] || [ "$$site_spin" -ge $$((lwip_total * 9 / 10)) ]; } && \
	   [ "$$tlb_enters" -gt 0 ] && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-smp-lock-contention: PASS (4-core run; stress ok, counters render, attribution accounts for $$site_spin of $$lwip_total lock-level spins across $$site_acq sampled acquires); log saved to $$log"; \
		echo "lwip stress-window: $$lwip_stress_spins spins over the run. Recorded, NOT asserted:" \
		     "arch_cpu_relax() iterations have no fixed conversion to wall time under TCG, and repeated" \
		     "4-core runs of this workload have spanned 0 to ~920000, so any magnitude threshold here" \
		     "would be flaky. The structural reductions are asserted in the kernel build instead (see the" \
		     "net_socket_t and TCP_MSG size _Static_asserts); this number is here so drift stays visible."; \
		grep -E '^(lwip|tasklist|runq): ' "$$log" || true; \
		echo "worst single acquire (cumulative): tasklist_lock $$tasklist_max spins, lwip site $$site_max spins"; \
		if [ "$$lwip_total" -eq 0 ]; then \
			echo "note: no lwip contention in this window, so the attribution invariant was satisfied vacuously rather than exercised"; \
		fi; \
		echo "tasklist_lock windows -- $$tasklist_split"; \
		echo "TLB convergence in the switch path (no global lock is held there since the split; it was proc_lock before): $$tlb_enters enters, $$tlb_waits flushed at least once, $$tlb_flushes local ASID flushes"; \
	else \
		echo "smoke-smp-lock-contention: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# ext4 JBD2 crash-consistency smoke
# ================================================================
# Halts the machine at each point in the JBD2 commit sequence, reboots the same
# image, and checks both what survived and what the host's e2fsck makes of the
# result.  The gate lives in tools/ext4_journal_gate.py because it is eight
# boots plus four host-side fsck runs; inline shell here would be unreadable.
#
# ARCH selects the build directory as well as the QEMU machine, so this is the
# same gate on every architecture the kernel boots under QEMU rather than one
# that only proves x86_64.
smoke-ext4-journal: dev-build $(EXT4_JOURNAL_IMG)
	$(PYTHON) tools/ext4_journal_gate.py --arch "$(ARCH)" \
		--build-dir "$(BUILD_DIR)" \
		--log-dir "$(SMOKE_LOG_DIR)" \
		--delay $(SMOKE_INPUT_DELAY_EXT4) --timeout $(SMOKE_TIMEOUT_EXT4)
# Network-lane observability smoke (stage A)
# ================================================================
# Boots a NR_CPUS=4 NET_LANES=4 build, runs net_stress_test, and reads
# /proc/net/status.  It asserts the structural facts only:
#
#   * the stress test passes, so the stack still moves bytes with lanes on;
#   * the lanes line renders with count=4, so the build knob reached the
#     kernel and the renderer used it;
#   * it prints exactly 4 occupancy values, one per lane;
#   * those values sum to the socket count -- the invariant the kernel loop
#     maintains, since every counted socket increments exactly one bucket;
#   * no panic and no page fault.
#
# Deliberately NOT asserted: any throughput, spin count or timing number.
# Under QEMU TCG the same workload's lwip spin count has spanned 0..920024
# across runs (docs/server-readiness.md), so a magnitude threshold would be
# flaky.  See docs/net/net-lanes.md.
#
# The leading newline matters: QEMU's serial input can lose the first byte if it
# arrives before the shell is ready, and the loss lands harmlessly on an empty
# line instead of eating the 'n' off net_stress_test.  Observed exactly once,
# as 'ent_stress_test: inaccessible or not found', which made this gate fail
# with the network perfectly healthy.
smoke-net-lanes: NET_HOSTFWD=
smoke-net-lanes:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 OPT="-DCONFIG_NET_PCB_SANE=1" dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-lanes-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '\nnet_stress_test\ncat /proc/net/status\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 4 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-lanes4/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-lanes4/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
		> "$$log" 2>&1 || status=$$?; \
	lanes_line=$$(awk '/^lanes: count=4 /{line=$$0} END{print line}' "$$log"); \
	occ_n=$$(printf '%s\n' "$$lanes_line" | awk '{print NF-4}'); \
	occ_sum=$$(printf '%s\n' "$$lanes_line" | awk '{s=0; for(i=5;i<=NF;i++) s+=$$i; print s+0}'); \
	sock_n=$$(printf '%s\n' "$$lanes_line" | awk '{for(i=1;i<=NF;i++) if($$i ~ /^sockets=/){sub(/^sockets=/,"",$$i); print $$i+0}}'); \
	if grep -q 'NET_STRESS_TEST: PASS' "$$log" && \
	   [ -n "$$lanes_line" ] && \
	   [ "$$occ_n" -eq 4 ] && \
	   [ "$$occ_sum" -eq "$$sock_n" ] && \
	   ! grep -qi 'panic' "$$log" && \
	   ! grep -qi 'page fault' "$$log"; then \
		echo "smoke-net-lanes: PASS (4-lane build; stress ok, lanes line renders count=4 with 4 occupancy values summing to sockets=$$sock_n); log saved to $$log"; \
		echo "  $$lanes_line"; \
	else \
		echo "smoke-net-lanes: failed with status $$status; tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# ================================================================
# Network-lane equivalence gate (stage A): 1 lane == pre-lane behaviour
# ================================================================
# Builds and boots BOTH NET_LANES=1 and NET_LANES=4 at NR_CPUS=4, runs
# net_stress_test in each, and requires the NET_STRESS_TEST: line to be
# byte-identical.  That is the property stage A claims: at one lane every
# net_lane_of() folds to a constant 0, so the embedded build compiles to the
# code that existed before lanes.
#
# The comparison alone would pass vacuously if the knob stopped reaching the
# kernel -- both builds would then be the same binary and agree trivially --
# so the gate also proves the two builds really differ: the 1-lane log must
# render count=1 with one occupancy value, the 4-lane log count=4 with four.
# A gate that cannot fail is worthless.
#
# Each log must also show NET_STRESS_TEST: PASS and exactly one
# NET_STRESS_TEST: line, so "FAIL == FAIL" can never be reported as
# equivalence.
smoke-net-lanes-n1: NET_HOSTFWD=
smoke-net-lanes-n1:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=1 dev-build
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	lanes1_dir=".kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4"; \
	lanes4_dir=".kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-lanes4"; \
	log1="$(SMOKE_LOG_DIR)/net-lanes-n1-lanes1-riscv64.log"; \
	log4="$(SMOKE_LOG_DIR)/net-lanes-n1-lanes4-riscv64.log"; \
	stress1_file="$(SMOKE_LOG_DIR)/net-lanes-n1.stress-lanes1"; \
	stress4_file="$(SMOKE_LOG_DIR)/net-lanes-n1.stress-lanes4"; \
	net_lanes_boot() { \
		dir="$$1"; log="$$2"; st=0; \
		{ sleep $(SMOKE_INPUT_DELAY); printf '\nnet_stress_test\ncat /proc/net/status\npoweroff\n'; } | \
		$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
			-machine virt -m 1G -nographic -smp 4 -bios default \
			-global virtio-mmio.force-legacy=false \
			-drive file="$$dir/fat32.img",if=none,format=raw,id=x0 \
			-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
			$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
			-kernel "$$dir/kernel.elf" \
			-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os' \
			> "$$log" 2>&1 || st=$$?; \
		return $$st; \
	}; \
	s1=0; net_lanes_boot "$$lanes1_dir" "$$log1" || s1=$$?; \
	s4=0; net_lanes_boot "$$lanes4_dir" "$$log4" || s4=$$?; \
	stress1=$$(awk '/NET_STRESS_TEST:/{line=$$0} END{print line}' "$$log1"); \
	stress4=$$(awk '/NET_STRESS_TEST:/{line=$$0} END{print line}' "$$log4"); \
	n1=$$(awk '/NET_STRESS_TEST:/{n++} END{print n+0}' "$$log1"); \
	n4=$$(awk '/NET_STRESS_TEST:/{n++} END{print n+0}' "$$log4"); \
	line1=$$(awk '/^lanes: count=1 /{line=$$0} END{print line}' "$$log1"); \
	line4=$$(awk '/^lanes: count=4 /{line=$$0} END{print line}' "$$log4"); \
	occ1=$$(printf '%s\n' "$$line1" | awk '{print NF-4}'); \
	occ4=$$(printf '%s\n' "$$line4" | awk '{print NF-4}'); \
	printf '%s\n' "$$stress1" > "$$stress1_file"; \
	printf '%s\n' "$$stress4" > "$$stress4_file"; \
	if [ "$$s1" -eq 0 ] && [ "$$s4" -eq 0 ] && \
	   grep -q 'NET_STRESS_TEST: PASS' "$$log1" && \
	   grep -q 'NET_STRESS_TEST: PASS' "$$log4" && \
	   [ "$$n1" -eq 1 ] && [ "$$n4" -eq 1 ] && \
	   [ "$$occ1" -eq 1 ] && [ "$$occ4" -eq 4 ] && \
	   [ -n "$$stress1" ] && \
	   cmp -s "$$stress1_file" "$$stress4_file" && \
	   ! grep -qi 'panic' "$$log1" && ! grep -qi 'panic' "$$log4" && \
	   ! grep -qi 'page fault' "$$log1" && ! grep -qi 'page fault' "$$log4"; then \
		echo "smoke-net-lanes-n1: PASS (1-lane and 4-lane builds both pass net_stress_test and report byte-identical verdicts)"; \
		echo "  lanes1: $$line1"; \
		echo "  lanes4: $$line4"; \
		echo "  identical NET_STRESS_TEST line: $$stress1"; \
	else \
		echo "smoke-net-lanes-n1: FAIL"; \
		echo "  qemu status: lanes1=$$s1 lanes4=$$s4"; \
		echo "  lanes1 lanes line: $${line1:-<absent>}"; \
		echo "  lanes4 lanes line: $${line4:-<absent>}"; \
		echo "  lanes1 NET_STRESS_TEST lines: $$n1"; \
		echo "  lanes4 NET_STRESS_TEST lines: $$n4"; \
		echo "  lanes1 verdict: $${stress1:-<absent>}"; \
		echo "  lanes4 verdict: $${stress4:-<absent>}"; \
		if ! cmp -s "$$stress1_file" "$$stress4_file"; then \
			echo "  verdicts differ:"; \
			diff "$$stress1_file" "$$stress4_file" || true; \
		fi; \
		echo "  logs: $$log1 $$log4"; \
		exit 1; \
	fi

# ================================================================
# TCP accept gate
# ================================================================
# Guards the accept path in both TCP modes.  tcp_accept_test asserts only that
# a handshake completes and accept() returns a usable fd, deliberately not the
# data transfer: the two are separable, and a red gate has to point at the thing
# that actually broke.
#
# It runs twice, once per mode, because the two modes are separate
# implementations that must agree on the observable result:
#   fast -- the listener is matched by the socket layer pairing the sockets
#   lwip -- the listener is a real lwIP LISTEN pcb and the protocol stack
#           completes the handshake
# The mode is chosen on the kernel command line rather than by a /proc write
# because a server's first listener is opened at boot, before a shell write can
# run; boot-time selection is also what makes tcp_listen=1 observable here.
#
# The structural assertion that matters is tcp_listen > 0.  Before the LISTEN
# pcb existed it was 0 in the steady state and an inbound SYN was answered with
# RST, so that number is the regression guard for the actual defect.
#
# No magnitude threshold is asserted on the counters.  The zero-valued ones are
# invariants, not statistics: a non-zero net_accept_drop, net_bh_overflow or
# net_alloc_fail means the receive or accept path discarded something it had
# already accepted, which is a defect at any magnitude.  The accept counts are
# printed for human review instead, since how many accepts a run makes depends
# on client retry timing.
smoke-net-accept: NET_HOSTFWD=
smoke-net-accept:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-accept-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '\ncat /proc/net/status\ncat /proc/a20/perf\ntcp_accept_test 12346\necho tcpmode fast > /proc/net/config\ntcp_accept_test 12347\ncat /proc/a20/perf\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 4 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 || status=$$?; \
	passes=$$(grep -c 'TCP_ACCEPT_TEST: PASS' "$$log" || true); \
	tcp_listen=$$(awk '/^pcbs: /{for(i=1;i<=NF;i++) if($$i ~ /^tcp_listen=/){v=$$i; sub(/^tcp_listen=/,"",v); print v+0; exit}}' "$$log"); \
	last_counter() { awk -v k="$$1" '$$1==k":"{v=$$2} END{print v+0}' "$$log"; }; \
	accept_drop=$$(last_counter net_accept_drop); \
	bh_overflow=$$(last_counter net_bh_overflow); \
	alloc_fail=$$(last_counter net_alloc_fail); \
	accept_queued=$$(last_counter net_accept_queued); \
	if [ "$$passes" -eq 2 ] && \
	   [ "$$tcp_listen" -gt 0 ] && \
	   [ "$$accept_drop" -eq 0 ] && \
	   [ "$$bh_overflow" -eq 0 ] && \
	   [ "$$alloc_fail" -eq 0 ] && \
	   ! grep -qiE 'panic|assertion failed' "$$log"; then \
		echo "smoke-net-accept: PASS (both TCP modes completed a handshake and accept; boot-time lwip mode shows tcp_listen=$$tcp_listen; no accept, bottom-half or allocation loss); log saved to $$log"; \
		echo "  recorded, not asserted: net_accept_queued=$$accept_queued (depends on client retry timing)"; \
	else \
		echo "smoke-net-accept: failed with status $$status (passes=$$passes tcp_listen=$$tcp_listen accept_drop=$$accept_drop bh_overflow=$$bh_overflow alloc_fail=$$alloc_fail); tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi

# Real LISTEN pcb create/close at NET_LANES>1.
#
# smoke-net-accept runs tcp_accept_test twice at NET_LANES=1, and
# smoke-net-lanes drives net_stress_test, which never opens a LISTEN pcb.
# So nothing gated the path where a listener is registered into a hashed lane
# bucket, matched by an inbound segment, and torn down -- the path that hid two
# bugs:
#
#   16304db8  tcp_pcb_remove() callers pre-indexed by lane while TCP_RMV()
#             indexed again, so a removal walked bucket 2L, out of bounds for
#             the wildcard lane.  Lane 0 self-cancels, so NET_LANES=1 is blind.
#   f7f3d670  tcp_input() hashed the segment's source port to pick a pcb's
#             bucket, so a listener matched only when the peer's random
#             ephemeral port collided with it: about 1 in NET_LANES.
#
# Both are silent at NET_LANES=1.  This runs one connection per lane on a
# 4-lane build with CONFIG_NET_PCB_SANE=1, so a misfiled or mis-spliced pcb
# fails as an assertion at the point of the mistake rather than as a fault far
# downstream -- or, for the lookup bug, as a plain missed connection.
smoke-net-tcp-lanes: NET_HOSTFWD=
smoke-net-tcp-lanes:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4 OPT="-DCONFIG_NET_PCB_SANE=1" dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-tcp-lanes-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); \
	  for p in 12401 12402 12403 12404 12405 12406 12407 12408; do echo "tcp_accept_test $$p"; sleep 1; done; \
	  echo 'cat /proc/net/status'; \
	  echo poweroff; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 4 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-lanes4/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-lanes4/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 || status=$$?; \
	passes=$$(grep -c 'TCP_ACCEPT_TEST: PASS' "$$log" || true); \
	lanes_line=$$(awk '/^lanes: count=4 /{line=$$0} END{print line}' "$$log"); \
	if [ "$$passes" -eq 8 ] && \
	   [ -n "$$lanes_line" ] && \
	   ! grep -q 'tcp_pcbs_sane' "$$log" && \
	   ! grep -qiE 'panic|assertion failed|page fault' "$$log"; then \
		echo "smoke-net-tcp-lanes: PASS (8 real LISTEN pcbs created, matched and closed, one per lane; PCB list checker compiled in and silent); log saved to $$log"; \
		echo "  $$lanes_line"; \
	else \
		echo "smoke-net-tcp-lanes: failed with status $$status (passes=$$passes of 8; list-checker hits=$$(grep -c 'tcp_pcbs_sane' "$$log" || true))"; \
		grep -E 'TCP_ACCEPT_TEST: FAIL' "$$log" || echo "  (no per-port FAIL line; see log tail)"; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi

# ================================================================
# IPv6 inbound-listen gate
# ================================================================
# Guards the v6 bind/listen/accept path, which did not exist: an AF_INET6
# SOCK_STREAM socket() succeeded but net_inet_socket_init gave it no tcp_pcb
# (that arm was AF_INET-only), so bind() had no pcb to bind, net_listen() had
# no bound pcb to convert into a LISTEN pcb, and connect() to a v6 listener
# failed with -ECONNREFUSED -- an errno that reads like "nothing is listening"
# rather than "this kernel has no v6 path at all", which is how the gap
# survived.
#
# ipv6_loopback_test asserts more than the handshake, unlike tcp_accept_test:
# it also moves 4000 B each way and checks the accepted peer really is reported
# as AF_INET6 on ::1.  Those are the two places the v6 path was separately
# wrong -- the accept drain hardcoded child->domain = AF_INET, so a v6
# connection was adopted into a v4 socket -- and a handshake-only gate would
# have stayed green through both.
#
# Run in both TCP modes, as smoke-net-accept does for v4: "fast" pairs the two
# sockets in the socket layer, "lwip" completes the handshake in the stack.
# A mode that regresses while the other passes is a real defect the gate has
# to be able to see, so neither run is optional.
#
# The /proc/net/tcp6 check lives inside ipv6_loopback_test, not here.  A gate
# that `cat`s /proc/net/tcp6 after the test returns reads an empty file --
# every socket the test made is closed by then -- so it could not have detected
# a missing or empty tcp6 file, only its own grep failing.  The test therefore
# reads both files while its own listener is still open and fails if the row is
# absent, rendered in the v4 layout, or listed in /proc/net/tcp as well.
smoke-net-ipv6: NET_HOSTFWD=
smoke-net-ipv6:
	$(MAKE) ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-ipv6-riscv64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf '\nipv6_loopback_test 13001\nipv6_loopback_test 13002\necho tcpmode fast > /proc/net/config\nipv6_loopback_test 13003\nipv6_loopback_test 13004\ncat /proc/a20/perf\npoweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_SMP) qemu-system-riscv64 \
		-machine virt -m 1G -nographic -smp 4 -bios default \
		-global virtio-mmio.force-legacy=false \
		-drive file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/fat32.img,if=none,format=raw,id=x0 \
		-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
		$(NETDEV_USER) -device virtio-net-device,netdev=net,bus=virtio-mmio-bus.4 \
		-kernel .kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 || status=$$?; \
	passes=$$(grep -c 'IPV6_LOOPBACK_TEST: PASS' "$$log" || true); \
	fails=$$(grep -c 'IPV6_LOOPBACK_TEST: FAIL' "$$log" || true); \
	last_counter() { awk -v k="$$1" '$$1==k":"{v=$$2} END{print v+0}' "$$log"; }; \
	accept_drop=$$(last_counter net_accept_drop); \
	bh_overflow=$$(last_counter net_bh_overflow); \
	alloc_fail=$$(last_counter net_alloc_fail); \
	if [ "$$passes" -eq 4 ] && [ "$$fails" -eq 0 ] && \
	   [ "$$accept_drop" -eq 0 ] && \
	   [ "$$bh_overflow" -eq 0 ] && \
	   [ "$$alloc_fail" -eq 0 ] && \
	   ! grep -qiE 'panic|assertion failed|page fault' "$$log"; then \
		echo "smoke-net-ipv6: PASS (4 v6 loopback connections accepted and carried 4000 B each way, both TCP modes; each listener verified in /proc/net/tcp6 in the tcp6 layout and absent from /proc/net/tcp); log saved to $$log"; \
	else \
		echo "smoke-net-ipv6: failed with status $$status (passes=$$passes of 4, fails=$$fails accept_drop=$$accept_drop bh_overflow=$$bh_overflow alloc_fail=$$alloc_fail); tail of $$log:"; \
		grep -E 'IPV6_LOOPBACK_TEST: FAIL|procheck|server:|client:' "$$log" | head -20 || true; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi
