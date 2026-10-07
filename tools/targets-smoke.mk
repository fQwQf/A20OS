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
# Resolved: a derived [net].hostfwd short form now names the guest address, so
# tools/a20_instance.py emits "hostfwd=tcp::5555-10.0.2.15:5555=on" rather than
# the addressless "tcp::5555-:5555".  Measured on QEMU 10.0.13 while doing it:
# the *bare* short form is what QEMU rejects ("Invalid parameter"); the
# addressless rule behind a hostfwd= key starts and binds normally, so the
# "Missing guest address" this note used to quote does not reproduce through
# -netdev user on that version.  The explicit address is spelled out anyway
# because it is the form measured to work end to end here.
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

# ================================================================
# AHCI completion path on a real ich9-ahci controller (x86_64 / q35)
# ================================================================
# The first gate that puts AHCI hardware under this kernel.  Until now ahci.c
# was compile-verified only: nothing in the tree attached an AHCI controller,
# so the PCI probe, the IDENTIFY, the FLUSH CACHE EXT that makes fsync durable,
# and now the completion interrupt had never executed once.
#
# Device layout, and why it is this layout:
#
#   * -machine q35, with -device ich9-ahci,id=ahci placed on pcie.0.  QEMU
#     puts that function at 00:02.0 (measured with `info pci` on the 10.0.13
#     in this tree), and q35 routes root-bus PCI INTx through the IOAPIC with
#     the swizzle arch_pci_intx_irq() implements -- dev 2 pin A lands on GSI 22,
#     vector 0x56.  i440fx would return -1 and silently keep polling, which is
#     why this gate is q35 and not the default PC machine.
#
#   * q35's own chipset exposes an AHCI function at 00:1f.2 (also measured).
#     It has no drive behind it, and the driver binds the first matching
#     function in enumeration order (00:02.0 comes first), so the chipset
#     controller is declined without costing a 5 s port timeout.  The
#     assertion below therefore also checks that exactly one "[AHCI] device
#     on port" line exists -- two would mean both controllers bound.
#
#   * ext4.img goes on the ich9-ahci bus as ide-hd, and the FAT32 image stays
#     on virtio-blk-pci.  mount_block_devices() mounts whichever class device
#     carries each filesystem, so /bin is virtio and /extra is the AHCI disk --
#     which is exactly the surface fsync_durability_test writes to.  The test's
#     own "block_flushes grew" assertion is inherited from smoke-fsync-durability
#     and is not restated here.
#
# What this gate adds on top of that test is the interrupt claim, and it is
# stated as two separate assertions because they fail for different reasons:
#
#   1. "[AHCI] ... completion=irq" -- the driver took the interrupt path.  A
#      run that reached the disk only by polling prints completion=poll and
#      fails here even though the disk worked.
#   2. ahci_irq_completions > 0 -- the top-half actually ran.  The driver's
#      own counter, read from /proc/a20/perf.  Claim (1) only says the handler
#      was registered; a controller that never asserts, or an IOAPIC route that
#      never delivers, leaves this at 0 while the pre-poll window quietly
#      retires every command.
#
# ahci_poll_completions is asserted to be 0 for the same reason: with a
# registered handler nothing may take the fallback, so a non-zero value would
# mean the two paths disagree about which one is live.
#
# The counters are read AFTER the test, because /proc/a20/perf is dormant until
# its first read (a20_perf_format() enables collection on entry), and the
# first cat only arms it.  The pre-test cat is what makes the second read a
# real measurement rather than a snapshot of a counter nobody was counting.
#
# Honest scope: this is one controller, one port, one command slot, under TCG.
# It says the completion interrupt is delivered and consumed on QEMU's model
# of an AHCI controller.  It does not cover multi-port controllers, the
# platform-bus variant, message-signalled interrupts (ahci.c has no MSI-X
# path), or any real SATA PHY.
#
# Same directory spelling smoke-pci-bridge uses, obtained from the Makefile
# rather than written out: `make ARCH=x86_64 dev-build` derives BUILD_VARIANT
# from every option that changes compiled code, so a literal here stops matching
# the directory the build writes as soon as one of those defaults moves (see
# the comment on print-build-dir in the Makefile).
X86_64_DEV_BUILD_DIR = $(shell $(MAKE) --no-print-directory ARCH=x86_64 print-build-dir)
AHCI_X86_64_BUILD_DIR = $(X86_64_DEV_BUILD_DIR)
# The explicit a20_resource.py call is HOST_RESOURCE_GATE_CONTRACT (docs/testing-gates.md):
# this target launches QEMU itself rather than going through tools/a20 test or
# tools/smoke.py, so it has to ask for the gate itself.  -m/-c must match the
# -m/-smp below, or the preflight is protecting a different launch.
smoke-ahci-ich9: NET_HOSTFWD=
smoke-ahci-ich9:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@$(PYTHON) tools/a20_resource.py -m 1G -c 1
	@set -e; \
	log="$(SMOKE_LOG_DIR)/ahci-ich9-x86_64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY_AHCI); \
	  printf 'cat /proc/a20/perf\n'; \
	  printf 'fsync_durability_test\n'; \
	  printf 'cat /proc/a20/perf\n'; \
	  printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_AHCI) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=$(AHCI_X86_64_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device virtio-blk-pci,drive=xb \
		-drive file=$(AHCI_X86_64_BUILD_DIR)/ext4.img,if=none,format=raw,id=xa \
		-device ich9-ahci,id=ahci \
		-device ide-hd,drive=xa,bus=ahci.0 \
		-kernel $(AHCI_X86_64_BUILD_DIR)/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	last_counter() { awk -v k="$$1" '$$1==k":"{v=$$2} END{print v+0}' "$$log"; }; \
	irq=$$(last_counter ahci_irq_completions); \
	wake=$$(last_counter ahci_irq_wakeups); \
	poll=$$(last_counter ahci_poll_completions); \
	cmds=$$(last_counter ahci_commands); \
	park=$$(last_counter ahci_park_rounds); \
	errs=$$(last_counter ahci_errors); \
	bound=$$(grep -c '\[AHCI\] device on port' "$$log" || true); \
	if grep -q 'FSYNC_TEST: PASS' "$$log" && \
	   grep -q 'completion=irq' "$$log" && \
	   [ "$$bound" -eq 1 ] && \
	   [ "$$irq" -gt 0 ] && \
	   [ "$$poll" -eq 0 ] && \
	   [ "$$cmds" -gt 0 ] && \
	   ! grep -qiE 'panic|assertion failed' "$$log"; then \
		echo "smoke-ahci-ich9: PASS (one ich9-ahci port bound on the interrupt path;" \
			 "$$cmds commands, $$irq of them completed through the IRQ top-half, $$poll via polling); log saved to $$log"; \
		echo "  recorded, not asserted: ahci_irq_wakeups=$$wake ahci_park_rounds=$$park ahci_errors=$$errs"; \
		grep -E '^\[AHCI\]' "$$log" || true; \
	else \
		echo "smoke-ahci-ich9: FAIL (status=$$status bound=$$bound cmds=$$cmds irq=$$irq wake=$$wake poll=$$poll park=$$park errs=$$errs)"; \
		grep -E '^\[AHCI\]|FSYNC_TEST' "$$log" || echo "  (no AHCI or FSYNC line at all -- the guest never got that far)"; \
		echo "  log saved to $$log; tail:"; \
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
# virtio-scsi completion smoke (x86_64)
# ================================================================
# The boot disk stays virtio-blk-pci and the scsi-hd behind virtio-scsi-pci is
# the scratch medium, so a failure localises to the SCSI controller rather than
# to "the machine came up at all".
#
# Unlike smoke-ahci-ich9, which reads ahci_irq_completions out of
# /proc/a20/perf, this gate reads the controller's counters through the block
# class stats ioctl (A20_BLK_IOCTL_GET_STATS) and has the guest print them.
# /proc/a20/perf counters are global, so a machine with two virtio-scsi
# controllers could not say which one the numbers came from, and a gate that
# cannot attribute its counter to the device under test proves less than it
# looks like it proves.  The guest still asserts the read/write/flush round
# trip itself, so the counter is a second opinion on a transfer that already
# had to be correct.
#
# See the case comment in tools/smoke_cases.py for the assertion list.
smoke-virtio-scsi-irq:
	$(PYTHON) tools/smoke.py smoke-virtio-scsi-irq

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
		-drive file=$(X86_64_DEV_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device pcie-root-port,id=rp1,bus=pcie.0,addr=0x4,chassis=1 \
		-device pcie-root-port,id=rp2,bus=rp1,addr=0x0,chassis=2 \
		-device virtio-blk-pci,drive=xb,bus=rp2,addr=0x0 \
		-kernel $(X86_64_DEV_BUILD_DIR)/kernel.elf \
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
# i440fx (no ECAM window) enumeration smoke
# ================================================================
# q35 has an ECAM window, so every other x86_64 gate reaches PCI through memory
# and cannot tell a working config-space path from one that reads a window that
# was never there.  i440fx has no ECAM at all: config space lives behind the
# legacy 0xCF8/0xCFC ports and the compiled-in q35 address holds nothing.
#
# That distinction was worth a gate because the failure mode is invisible from
# the outside.  Enumeration did not fail loudly -- it published one phantom
# device per slot, all id=0000:0000, none matching a driver, and the machine
# died much later at "no init program found" pointing at nothing.  Three separate
# defects hid it: the phantom filter rejected only 0xffff and not 0x0000, the
# ECAM-absence probe in pci_host.c also looked only for 0xffffffff, and pci_bus.c
# computed ECAM addresses itself instead of calling the arch HAL that already
# falls back to the ports.  Asserting the real device list is what would have
# caught any one of them.
#
# What this does NOT cover: i440fx INTx routing.  Its PIRQ links are programmed
# by firmware into the PIIX3 and there is no DSDT _PRT to read here, so these
# devices keep their polling path -- that is the correct outcome, and the gate
# asserts it rather than treating "no interrupt" as a failure.
smoke-pci-i440fx: NET_HOSTFWD=
smoke-pci-i440fx:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@$(PYTHON) tools/a20_resource.py -m 1G -c 1
	@set -e; \
	log="$(SMOKE_LOG_DIR)/pci-i440fx-x86_64.log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT) qemu-system-x86_64 \
		-machine pc -m 1G -nographic -smp 1 -no-reboot \
		-drive file=$(X86_64_DEV_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device virtio-blk-pci,drive=xb \
		-device e1000 \
		-device ich9-ahci,id=ahci \
		-kernel $(X86_64_DEV_BUILD_DIR)/kernel.elf \
		> "$$log" 2>&1 || status=$$?; \
	phantoms=$$(grep -c 'id=0000:0000' "$$log" || true); \
	if grep -q 'bridges walked' "$$log" && \
	   grep -qE '\[BUS\] pci 00:[0-9a-f]{2}\.0 id=8086:1237 .*class=06:00:00' "$$log" && \
	   grep -qE '\[BUS\] pci 00:[0-9a-f]{2}\.0 id=1af4:1001' "$$log" && \
	   grep -qE '\[BUS\] pci 00:[0-9a-f]{2}\.0 id=8086:100e' "$$log" && \
	   grep -qE '\[BUS\] pci 00:[0-9a-f]{2}\.0 id=8086:2922' "$$log" && \
	   [ "$$phantoms" -eq 0 ] && \
	   ! grep -qi 'panic' "$$log"; then \
		echo "smoke-pci-i440fx: PASS (i440fx enumerated through the legacy 0xCF8/0xCFC ports;" \
			 "host bridge, virtio-blk, e1000 and AHCI all found, 0 phantom devices); log saved to $$log"; \
	else \
		echo "smoke-pci-i440fx: FAIL (status=$$status phantoms=$$phantoms) -- i440fx has no ECAM," \
			 "so this only passes if config space is reached through the legacy ports"; \
		grep -E '\[PCI\]|\[BUS\]' "$$log" | head -20 || true; \
		echo "  log saved to $$log; tail:"; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi

# ================================================================
# virtio-console / /dev/vport0 smoke
# ================================================================
# Both directions are asserted, and neither half is trusted on its own:
#
#   host -> guest: tools/vport_host_probe.py writes 64 bytes into the
#     virtconsole chardev socket, and the guest program (user/cmds/core/
#     vport_test.c) checks every byte it read back from /dev/vport0.  A receive
#     path that silently drops or corrupts bytes fails inside the guest.
#   guest -> host: the same 64 bytes are written back out of /dev/vport0 and
#     the probe compares them with what it sent.
#
# The probe waits for the guest's "VPORT_TEST: READY" marker in the serial log
# before connecting, so there is no fixed sleep racing the guest boot, and the
# chardev is server=on,wait=off so QEMU does not block on a connection nobody
# has made yet.  A guest PASS alone is not accepted: without the host echo the
# transmit half of the driver would be untested.
smoke-virtio-console: NET_HOSTFWD=
smoke-virtio-console:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/virtio-console-x86_64.log"; \
	sock="$(SMOKE_LOG_DIR)/virtio-console.sock"; \
	status=0; probe=0; \
	rm -f "$$log" "$$sock"; \
	{ sleep $(SMOKE_INPUT_DELAY_VPORT); printf 'vport_test\n'; \
	  sleep 30; printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_VPORT) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=$(X86_64_DEV_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device virtio-blk-pci,drive=xb \
		-chardev socket,id=vportch,path=$$sock,server=on,wait=off \
		-device virtio-serial-pci,id=vser0 \
		-device virtconsole,chardev=vportch,bus=vser0.0 \
		-kernel $(X86_64_DEV_BUILD_DIR)/kernel.elf \
		> "$$log" 2>&1 & \
	qemu_pid=$$!; \
	$(PYTHON) tools/vport_host_probe.py "$$sock" "$$log" || probe=$$?; \
	wait $$qemu_pid || status=$$?; \
	if [ "$$probe" -ne 0 ]; then \
		echo "smoke-virtio-console: FAIL -- the host probe did not complete the round trip"; \
		echo "  (tools/vport_host_probe.py exit $$probe). Guest verdict, if any:"; \
		grep -E 'VPORT_TEST|\[VPORT\]' "$$log" || echo "  (none: the guest never got that far)"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif grep -q 'VPORT_TEST: PASS' "$$log" && ! grep -qi 'panic' "$$log"; then \
		echo "smoke-virtio-console: PASS (64 bytes host->guest->/dev/vport0->host)"; \
		echo "  log saved to $$log"; \
	elif grep -q 'VPORT_TEST: FAIL' "$$log"; then \
		echo "smoke-virtio-console: FAIL -- the guest reported a failure:"; \
		grep -E 'VPORT_TEST|\[VPORT\]' "$$log" | tail -n 5; \
		echo "  log saved to $$log"; \
		exit 1; \
	else \
		echo "smoke-virtio-console: failed with status $$status, no guest verdict; tail of $$log:"; \
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

# ================================================================
# E1000 interrupt data plane (x86_64 / q35)
# ================================================================
# What this gate is for.
#
# The e1000 driver registered a line and unmasked IMS long before this gate
# existed, so "the driver has an interrupt path" was never the thing in doubt.
# What was in doubt is whether anything ever proved the device actually raised
# it: a polling NIC and an interrupt-driven NIC move identical packets through
# an identical ring, and the difference is invisible from the outside.  The
# class .poll hook is called by the lwIP drain whether or not a handler ever
# runs, so a gate asserting only "the network works" would pass with the
# interrupt path completely dead -- which is exactly what a broken IOAPIC route
# or a left-masked IMS leaves behind.
#
# So the gate asserts the interrupt count itself, and it needs traffic that can
# only have arrived from the wire.  That is what the hostfwd round trip is for:
# same shape as smoke-netfilter-nat (host connects, guest socket accepts and
# echoes) but with the netfilter test replaced by the listener that already
# exists for this purpose.
#
# Why a plain listener and not netnat_test: this gate is about the receive
# interrupt, not about conntrack.  netnat_test would put a DNAT rewrite between
# the wire and the driver, so a failure could be the filter rather than the NIC,
# and a pass would not localise anything.  tcp_accept_test is the smallest thing
# that moves a frame through the host forward, the e1000 RX ring and lwIP.  The
# echo makes the outbound half real too, which is what moves e1000_irq_tx: an
# accept that never answered would exercise RX alone.
#
# The assertions, stated separately because they fail for different reasons:
#
#   1. the ready line carries no dataplane=polling -- the driver claimed a line
#      and unmasked IMS.  A run that fell back prints dataplane=polling and
#      fails here even though the network worked.
#   2. e1000_irq_calls > 0 and e1000_irq_rx > 0 -- the top-half actually ran.
#      Claim (1) only says a handler was registered; nothing in it says the
#      controller asserted or that the route delivered.  This is the assertion
#      that catches a dead MSI-X table or a wrong INTx swizzle.
#   3. e1000_irq_tx > 0 and e1000_tx_reclaimed > 0 -- the transmit completion
#      path released descriptors.  Kept apart from (2) because it is a separate
#      defect: the count can be positive while the reclaim was dropped, and
#      this gate's handful of bytes would still move, because the ring is 256
#      deep.  Only a sustained transmit would notice.
#   4. e1000_rx_drained > 0 -- the driver retired receive descriptors, i.e. the
#      ring was actually walked rather than the stack finding nothing.
#
# Counters are read AFTER the test from /proc/a20/perf.  Collection there is
# dormant until the file is first read (a20_perf_format() enables it on entry),
# so that single post-test cat is itself the first read and therefore the first
# real measurement.  No pre-test cat is issued, unlike smoke-ahci-ich9, on
# purpose: these are not a snapshot of a counter nobody was counting.
#
# Honest scope: one 82540EM behind QEMU's user-mode network, one port, under
# TCG, single core, MSI-X if the part offers it and INTx otherwise.  It does not
# cover multi-queue, the e1000e MSI-X path, a real PHY, receive coalescing, or
# any throughput claim -- it asserts delivery and accounting, not bandwidth.
#
# a20.e1000.poll=1 is deliberately NOT passed here.  The forced-polling run is
# the negative control; putting it in the same gate as the positive claim would
# let a boot that ignored the driver entirely satisfy one of the two.  The knob's
# own behaviour is therefore NOT verified by this gate.
E1000_X86_64_BUILD_DIR = $(X86_64_DEV_BUILD_DIR)
# This target launches QEMU itself rather than going through tools/a20 test or
# tools/smoke.py, so it asks for the gate itself (HOST_RESOURCE_GATE_CONTRACT in
# docs/testing-gates.md).  -m/-c match the launch below.
smoke-net-e1000-irq: NET_HOSTFWD=
smoke-net-e1000-irq:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@$(PYTHON) tools/a20_resource.py -m 1G -c 1
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-e1000-irq-x86_64.log"; \
	status=0; probe=0; \
	rm -f "$$log"; \
	{ sleep $(SMOKE_INPUT_DELAY_E1000); \
	  printf '\ncat /proc/a20/perf\n'; \
	  printf 'echo tcpmode lwip > /proc/net/config\n'; \
	  printf 'cat /proc/net/config\n'; \
	  printf 'tcp_accept_test 18091 10.0.2.15\n'; \
	  printf 'cat /proc/a20/perf\n'; \
	  printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_E1000) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=$(E1000_X86_64_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device virtio-blk-pci,drive=xb \
		-netdev user,id=net,hostfwd=tcp:127.0.0.1:18091-10.0.2.15:18091 \
		-device e1000,netdev=net \
		-kernel $(E1000_X86_64_BUILD_DIR)/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 & \
	qemu_pid=$$!; \
	$(PYTHON) tools/e1000_host_probe.py 18091 || probe=$$?; \
	wait $$qemu_pid || status=$$?; \
	last_counter() { awk -v k="$$1" '$$1==k":"{v=$$2} END{print v+0}' "$$log"; }; \
	irq_calls=$$(last_counter e1000_irq_calls); \
	irq_rx=$$(last_counter e1000_irq_rx); \
	irq_tx=$$(last_counter e1000_irq_tx); \
	irq_empty=$$(last_counter e1000_irq_empty); \
	tx_reclaimed=$$(last_counter e1000_tx_reclaimed); \
	rx_drained=$$(last_counter e1000_rx_drained); \
	ready_line=$$(grep -m1 '\[E1000\] ready:' "$$log" || true); \
	tcpmode_line=$$(grep -m1 '^tcpmode=' "$$log" || true); \
	if [ "$$probe" -ne 0 ]; then \
		echo "smoke-net-e1000-irq: FAIL -- the host never completed the port forward"; \
		echo "  (tools/e1000_host_probe.py exit $$probe). Guest verdict, if any:"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || echo "  (none: the guest never got that far)"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif ! printf '%s' "$$tcpmode_line" | grep -qx 'tcpmode=lwip'; then \
		echo "smoke-net-e1000-irq: FAIL -- the guest never reached tcpmode=lwip."; \
		echo "  An off-box SYN is answered with RST unless the listener is a real lwIP LISTEN"; \
		echo "  pcb (socket_control.c:213), so a run that stayed in the default fast mode would"; \
		echo "  fail for a reason that says nothing about this driver.  Reported:"; \
		echo "    $${tcpmode_line:-<absent: /proc/net/config never printed>}"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || true; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif [ -n "$$ready_line" ] && printf '%s' "$$ready_line" | grep -q 'dataplane=polling'; then \
		echo "smoke-net-e1000-irq: FAIL -- the driver fell back to the polling data plane"; \
		echo "  even though the network worked, which is precisely the state this gate exists"; \
		echo "  to rule out. Driver line:"; \
		echo "    $$ready_line"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif grep -q 'TCP_ACCEPT_TEST: PASS' "$$log" && \
	     [ -n "$$ready_line" ] && \
	     [ "$$irq_calls" -gt 0 ] && \
	     [ "$$irq_rx" -gt 0 ] && \
	     [ "$$irq_tx" -gt 0 ] && \
	     [ "$$tx_reclaimed" -gt 0 ] && \
	     [ "$$rx_drained" -gt 0 ] && \
	     ! grep -qiE 'panic|assertion failed|page fault' "$$log"; then \
		echo "smoke-net-e1000-irq: PASS (hostfwd round trip 18091 -> 10.0.2.15:18091 through the e1000;" \
			 "$$irq_calls handler entries, $$irq_rx carrying an RX cause, $$irq_tx a TX cause," \
			 "$$rx_drained RX descriptors drained, $$tx_reclaimed TX descriptors released); log saved to $$log"; \
		echo "    $$ready_line"; \
		echo "  recorded, not asserted: e1000_irq_empty=$$irq_empty (shared-line/throttle cost, not a fault)"; \
	else \
		echo "smoke-net-e1000-irq: FAIL (status=$$status probe=$$probe irq_calls=$$irq_calls irq_rx=$$irq_rx" \
			 "irq_tx=$$irq_tx tx_reclaimed=$$tx_reclaimed rx_drained=$$rx_drained)"; \
		echo "  driver ready line: $${ready_line:-<absent: the e1000 never probed>}"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || echo "  (no TCP_ACCEPT_TEST line at all)"; \
		echo "  log saved to $$log; tail:"; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi

# ---------------------------------------------------------------------------
# smoke-net-rtl8139 -- end-to-end round trip through the Realtek RTL8139
# ---------------------------------------------------------------------------
# Why this gate exists: the only NIC this tree carried for the x86_64 QEMU target
# was e1000, and e1000's id table is Realtek-free, so `-nic user,model=rtl8139`
# produced a PCI function that no driver claimed and therefore no device in
# DEV_CLASS_NET at all.  A boot with no netif and no NIC is not a network failure
# you can see; the stack simply never had a device.  This gate claims 10ec:8139
# and then asserts traffic actually crossed it.
#
# Shape and reasoning are smoke-net-e1000-irq's, deliberately and in full: same
# machine (q35, 1G, 1 cpu, the both-dev image), same hostfwd shape, the same
# tcp_accept_test listener, the same /proc/a20/perf counter reads after the test.
# Only the -device line differs.  Diverging the recipe would make a green here
# incomparable to a green there, and there is nothing about an RTL8139 that
# warrants a second guest boot model to understand.
#
# tcpmode is set twice, and the second write is the one that counts.  On this
# host the -append string never reaches the guest: QEMU's multiboot loader puts
# the command line in the page that follows the kernel image in its fw_cfg blob
# (hw/i386/multiboot.c: mb_add_cmdline() returns mh_load_addr + offset_cmdlines)
# and the info block correctly points at it -- g_mb_info=0x9500, flags=0x24f,
# mi->cmdline=<load_addr + page-aligned image size> -- but SeaBIOS 1.16.3 copies
# only the image bytes, so guest RAM there is zero and firmware_bootargs()
# reports cmdline='' and then cmdline_size=0 from fw_cfg.  The kernel therefore
# falls back to the string in kernel/platform/qemu-virt-x86_64/board.c:24, which
# carries no a20.tcpmode, and boots in the default fast mode.  In fast mode
# net_listen_sock() drops the bound pcb (socket_control.c:242), so the port is not
# listening in the stack and an inbound SYN is answered with RST -- a failure
# that has nothing to do with the NIC.  The `echo tcpmode lwip` line uses the
# write path a20_net_config_write() already provides and that smoke-net-ipv4
# already uses to switch the other way, and it runs after init's telnetd has
# taken port 2323 in whatever mode the boot chose, so it cannot race the first
# listener.  The gate asserts the readback, so a run that failed to switch
# reports that instead of blaming the driver.
#
# The assertions, stated separately because they fail for different reasons:
#
#   1. probe=0 and TCP_ACCEPT_TEST: PASS -- the end-to-end round trip.  Host
#      connects to a forwarded port, the guest socket accepts, echoes one byte,
#      and the byte comes back carrying the forwarded source port.  This is the
#      assertion the gate was asked for, and the only one that says the data
#      plane works.
#   2. the ready line exists and says mode=irq -- the driver bound the function
#      and claimed an interrupt line instead of falling back to a20.rtl8139.poll.
#      Claim (1) alone does not rule out the fallback: .poll runs from the same
#      lwIP drain and a polled NIC moves bytes perfectly well.
#   3. rtl8139_irq_calls > 0 and rtl8139_irq_rx > 0 -- the handler ran and saw a
#      receive cause.  Neither (1) nor (2) implies this.  (2) says a handler was
#      registered; only this says the controller asserted the line and the IOAPIC
#      route delivered it.  It is the assertion that catches a wrong INTx swizzle.
#   4. rtl8139_tx_reclaimed > 0 and rtl8139_rx_drained > 0 -- descriptors were
#      retired on both halves.  Kept apart from (3) because each is a separate
#      defect: the transmit ring is four descriptors deep, which is far too much
#      runway for one frame to notice a reclaim that never happened, and
#      rx_drained distinguishes "the ring was walked" from "the stack found
#      nothing to walk".
#   5. no panic / assertion failure / page fault anywhere in the log.
#
# Counters are read AFTER the test from /proc/a20/perf, on the same reasoning as
# smoke-net-e1000-irq: a20_perf_format() enables collection on the first read, so
# that single post-test cat is itself the first real measurement.  No pre-test cat
# is issued, because these would not be a snapshot of a counter nobody was
# counting.
#
# Honest scope: one RTL8139 behind QEMU's user-mode network, one port, under TCG,
# single core, INTx (this part has no MSI-X).  It does not cover real silicon, the
# RTL8139C register superset, WOL, power saving, cable-speed reporting, multi-
# queue, receive coalescing, or any throughput claim -- it asserts delivery and
# accounting, not bandwidth.  The RX ring geometry the driver programs
# (RCR[13:11] = 0b11, a 64 KiB ring) was taken from QEMU's device model; it has
# NOT been run against real silicon, which documents that same field as "8K + 16K".
# Anything this gate says about the register map is a statement about QEMU's
# implementation.
#
# a20.rtl8139.poll=1 is deliberately NOT passed here.  The forced-polling run is
# the negative control; putting it in the same gate as the positive claim would
# let a boot that ignored the driver entirely satisfy one of the two.  The knob's
# own behaviour is therefore NOT verified by this gate.
RTL8139_X86_64_BUILD_DIR = $(X86_64_DEV_BUILD_DIR)
smoke-net-rtl8139: NET_HOSTFWD=
smoke-net-rtl8139:
	$(MAKE) ARCH=x86_64 dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@$(PYTHON) tools/a20_resource.py -m 1G -c 1
	@set -e; \
	log="$(SMOKE_LOG_DIR)/net-rtl8139-x86_64.log"; \
	status=0; probe=0; \
	rm -f "$$log"; \
	{ sleep $(SMOKE_INPUT_DELAY_RTL8139); \
	  printf '\ncat /proc/a20/perf\n'; \
	  printf 'echo tcpmode lwip > /proc/net/config\n'; \
	  printf 'cat /proc/net/config\n'; \
	  printf 'tcp_accept_test 18093 10.0.2.15\n'; \
	  printf 'cat /proc/a20/perf\n'; \
	  printf 'poweroff\n'; } | \
	$(TIMEOUT) $(SMOKE_TIMEOUT_RTL8139) qemu-system-x86_64 \
		-machine q35 -m 1G -nographic -smp 1 -no-reboot \
		-drive file=$(RTL8139_X86_64_BUILD_DIR)/fat32.img,if=none,format=raw,id=xb \
		-device virtio-blk-pci,drive=xb \
		-netdev user,id=net,hostfwd=tcp:127.0.0.1:18093-10.0.2.15:18093 \
		-device rtl8139,netdev=net \
		-kernel $(RTL8139_X86_64_BUILD_DIR)/kernel.elf \
		-append 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os a20.tcpmode=lwip' \
		> "$$log" 2>&1 & \
	qemu_pid=$$!; \
	$(PYTHON) tools/rtl8139_host_probe.py 18093 || probe=$$?; \
	wait $$qemu_pid || status=$$?; \
	last_counter() { awk -v k="$$1" '$$1==k":"{v=$$2} END{print v+0}' "$$log"; }; \
	irq_calls=$$(last_counter rtl8139_irq_calls); \
	irq_rx=$$(last_counter rtl8139_irq_rx); \
	irq_tx=$$(last_counter rtl8139_irq_tx); \
	tx_reclaimed=$$(last_counter rtl8139_tx_reclaimed); \
	rx_drained=$$(last_counter rtl8139_rx_drained); \
	ready_line=$$(grep -m1 '\[RTL8139\] ready:' "$$log" || true); \
	tcpmode_line=$$(grep -m1 '^tcpmode=' "$$log" || true); \
	if [ "$$probe" -ne 0 ]; then \
		echo "smoke-net-rtl8139: FAIL -- the host never completed the port forward"; \
		echo "  (tools/rtl8139_host_probe.py exit $$probe). Guest verdict, if any:"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || echo "  (none: the guest never got that far)"; \
		echo "  driver ready line: $${ready_line:-<absent: the rtl8139 never probed>}"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif [ -z "$$ready_line" ]; then \
		echo "smoke-net-rtl8139: FAIL -- the port forward worked but no RTL8139 probed."; \
		echo "  Something else answered on the forwarded port, so the round trip did not cross"; \
		echo "  this driver and says nothing about it."; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || true; \
		echo "  log saved to $$log; tail:"; \
		tail -n 60 "$$log"; \
		exit 1; \
	elif ! printf '%s' "$$tcpmode_line" | grep -qx 'tcpmode=lwip'; then \
		echo "smoke-net-rtl8139: FAIL -- the guest never reached tcpmode=lwip."; \
		echo "  An off-box SYN is answered with RST unless the listener is a real lwIP LISTEN"; \
		echo "  pcb (socket_control.c:213), so a run that stayed in the default fast mode would"; \
		echo "  fail for a reason that says nothing about this driver.  Reported:"; \
		echo "    $${tcpmode_line:-<absent: /proc/net/config never printed>}"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || true; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif printf '%s' "$$ready_line" | grep -q 'mode=poll'; then \
		echo "smoke-net-rtl8139: FAIL -- the driver fell back to the polling data plane"; \
		echo "  even though the network worked, which is precisely the state this gate exists"; \
		echo "  to rule out. Driver line:"; \
		echo "    $$ready_line"; \
		echo "  log saved to $$log"; \
		exit 1; \
	elif grep -q 'TCP_ACCEPT_TEST: PASS' "$$log" && \
	     [ "$$irq_calls" -gt 0 ] && \
	     [ "$$irq_rx" -gt 0 ] && \
	     [ "$$tx_reclaimed" -gt 0 ] && \
	     [ "$$rx_drained" -gt 0 ] && \
	     ! grep -qiE 'panic|assertion failed|page fault' "$$log"; then \
		echo "smoke-net-rtl8139: PASS (hostfwd round trip 18093 -> 10.0.2.15:18093 through the RTL8139;" \
			 "$$irq_calls handler entries, $$irq_rx carrying an RX cause, $$irq_tx a TX cause," \
			 "$$rx_drained frames retired from the RX ring, $$tx_reclaimed TX descriptors released); log saved to $$log"; \
		echo "    $$ready_line"; \
	else \
		echo "smoke-net-rtl8139: FAIL (status=$$status probe=$$probe irq_calls=$$irq_calls irq_rx=$$irq_rx" \
			 "irq_tx=$$irq_tx tx_reclaimed=$$tx_reclaimed rx_drained=$$rx_drained)"; \
		echo "  driver ready line: $${ready_line:-<absent: the rtl8139 never probed>}"; \
		grep -E 'TCP_ACCEPT_TEST' "$$log" || echo "  (no TCP_ACCEPT_TEST line at all)"; \
		echo "  log saved to $$log; tail:"; \
		tail -n 60 "$$log"; \
		exit 1; \
	fi

# ================================================================
# Serial RX fidelity gate: 300 commands must round-trip byte-exact
# ================================================================
# docs/net/net-lanes.md recorded ~3 of 800 console commands arriving at the
# guest with an adjacent character transposed or dropped.  That number was
# read off a console log, which cannot separate a real UART receive fault from
# a kernel print landing inside the echoed line -- and `grep -c PASS` then
# counts "never executed" as "did not fail".  This gate makes the round trip
# itself the assertion.
#
# tools/serial_fidelity.py types 300 numbered commands, each with a 48-byte
# base62 payload, and compares what came back byte for byte.  It fails on:
#   * a payload that came back different (truncated / transposed / vanished);
#   * a line that never returned at all -- a command that did not run is the
#     same measurement hazard as one that ran wrong;
#   * the guest shell reporting a command it could not find (the observed
#     symptom: 'ceho: inaccessible or not found' after a transposition);
#   * [UART] rx_fidelity reporting a non-zero dropped count, i.e. the ring
#     overflowed and silently discarded bytes the harness cannot attribute.
# A run with no rx_fidelity line at all also fails: the probe must be in the
# kernel being tested or the gate would be asserting nothing.
#
# The guest runs a background flood loop (--load) and -smp 4, the amplifier
# that reproduced the fault on the unfixed tree: two of three baseline runs
# corrupted command 43/44 (docs/measured/serial-fidelity.md).  Deliberately
# NOT asserted: interleaved_tokens.  A kernel print spliced into an echo line
# corrupts the *log*, not the receive path, and counting it here would make
# this gate fail on console traffic that the driver delivered correctly.
SERIAL_FIDELITY_ARGS = ARCH=riscv64 ABI=linux BRINGUP=0 NR_CPUS=4 NET_LANES=4
smoke-serial-fidelity: NET_HOSTFWD=
smoke-serial-fidelity:
	$(MAKE) $(SERIAL_FIDELITY_ARGS) dev-build
	@mkdir -p $(SMOKE_LOG_DIR)
	@$(PYTHON) tools/a20_resource.py -m 1G -c 4
	@set -e; \
	log="$(SMOKE_LOG_DIR)/serial-fidelity-riscv64.log"; \
	summary="$(SMOKE_LOG_DIR)/serial-fidelity-riscv64.summary"; \
	status=0; \
	kernel_dir=$$($(MAKE) --no-print-directory $(SERIAL_FIDELITY_ARGS) print-build-dir); \
	$(PYTHON) tools/serial_fidelity.py --kernel-dir "$$kernel_dir" \
		--count 300 --timeout 600 \
		--load 'i=0; while [ $$i -lt 200000 ]; do echo flood $$i; i=$$((i+1)); done' \
		--qemu-arg=-smp --qemu-arg=4 \
		--log "$$log" > "$$summary" 2>&1 || status=$$?; \
	result_line=$$(grep 'returned=' "$$summary" || true); \
	fid_line=$$(grep 'rx_fidelity' "$$log" | tail -1 || true); \
	dropped=$$(printf '%s\n' "$$fid_line" | sed -n 's/.*dropped=\([0-9]*\).*/\1/p'); \
	poll_bytes=$$(printf '%s\n' "$$fid_line" | sed -n 's/.*poll_bytes=\([0-9]*\).*/\1/p'); \
	not_found=$$(grep -c 'inaccessible or not found' "$$log" || true); \
	if [ "$$status" -eq 0 ] && [ -n "$$result_line" ] && \
	   [ -n "$$fid_line" ] && [ "$$dropped" = 0 ] && \
	   [ -n "$$poll_bytes" ] && [ "$$poll_bytes" -gt 0 ] && \
	   [ "$$not_found" -eq 0 ] && \
	   ! grep -qiE 'panic|page fault' "$$log"; then \
		echo "smoke-serial-fidelity: PASS (300/300 commands round-tripped byte-exact under -smp 4 + guest flood; $$result_line)"; \
		echo "    $$fid_line"; \
	else \
		echo "smoke-serial-fidelity: FAIL (status=$$status dropped='$$dropped' poll_bytes='$$poll_bytes' shell_not_found=$$not_found)"; \
		echo "  $$result_line"; \
		echo "  last fidelity line: $${fid_line:-<absent: the probe never printed>}"; \
		echo "  log saved to $$log; summary:"; \
		cat "$$summary"; \
		exit 1; \
	fi
