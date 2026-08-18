# LS2K1000 Porting Guide for Agents

## Mission

Continue the A20OS port to the Loongson LS2K1000-DP-V10 while preserving two
known-good recovery paths:

1. The board must continue to boot its vendor Linux after a physical reset.
2. The `qemu-virt-loongarch64` build and shell must remain functional.

Work on the physical board incrementally. Keep a working RAM-only image at
each milestone and do not combine timer, interrupt-controller, network,
storage, and SMP bring-up into one untestable change.

This file applies to the entire repository.

## Current Branch and Baseline

- Active development branch: `dev/2k1000`.
- First working physical-board shell baseline: commit `86a847fd`
  (`loongarch: bring up LS2K1000 RAM shell`).
- Board: LS2K1000-DP-V10, Loongson-2K1001/LA264, 1 GiB RAM.
- Firmware observed on hardware: U-Boot 2022.04-v2.1.0-00583-g2ed41674.
- Serial console observed in WSL: `/dev/ttyUSB0` for the latest run, 115200
  8N1. The `ttyUSB` number can change after USB/IP reattachment; resolve the
  current CH340 device before opening minicom.

The 2026-08-18 physical-board test reached a stable `mksh` prompt from a
RAM-loaded image. The following commands completed successfully on hardware:

```text
help
cat /etc/os-release
ps
```

The same image path exercised PLV3 entry, four-level page-table walking, TLB
refill, Linux syscall dispatch, `fork`, `execve`, `exit`, and `wait`. This is a
RAM-shell milestone, not a complete board port.

The exact artifact tested on 2026-08-18 was 2,608,920 bytes with SHA-256:

```text
b2a62a40d24ef7ef95e885921988d1794cb1a3aa9f92c0282fcf5515509cf561
```

It was stored as `/boot/a20-shellfix.bin` on the vendor Linux filesystem. This
hash records the known-good artifact; newly built images are expected to have
different hashes and must be checked before each upload.

The first non-cooperative timer experiment was also RAM-booted on 2026-08-18:

```text
/boot/a20-ls2k-timer-phase1-20260818.bin
size: 2,613,016 bytes
SHA-256: e5840999ba35cfcf6fa68215c02df4b2cd8a7e653b543a9a27a56fa7b994bc71
log: /tmp/a20-ls2k-timer-phase1-board-run-20260818.log
```

It printed `LS2K1000 timer interrupts enabled`, reached `mksh`, and ran
`help` twice, `cat /etc/os-release`, and `ps` without a timer-trap hang. This
validates repeated local timer delivery and exception return for the shell
path, but not the complete Phase 1 acceptance criteria. Bulk serial input
exposed receive loss because the polling UART slept for 50 ms without a
device IRQ. A subsequent Ctrl-C test terminated the minimal init and halted
the board. A physical reset then followed the unchanged default U-Boot path,
loaded `/boot/uImage`, and returned to the vendor Linux 5.10 root prompt.

The timer/sleep candidate embeds `/bin/sleep` and gives the LS2K1000 polling
UART a 1 ms wake interval. It is build-, QEMU-, and partially physical-board-
validated:

```text
/home/gyy/a20-ls2k-timer-sleep-poll-20260818.bin
size: 2,686,776 bytes
SHA-256: 43bdcacb6cfb837a689d749b1f563b018cf0524fc11e8c17289dda4ddc2f7eb8
QEMU log: /tmp/a20-qemu-la64-timer-sleep-20260818.log
board log: /tmp/a20-ls2k-timer-sleep-poll-board-run-20260818.log
```

On hardware it reached `mksh` with timer interrupts enabled and completed
`help`, `cat /etc/os-release`, `ps`, `sleep 1; echo SLEEP_OK`, and
`sleep 2; echo SLEEP2_OK` when commands were entered at a controlled rate.
This validates local timer delivery, repeated exception return, and timed
sleep/wakeup through the PLV3 shell. It does not validate timeout coverage,
idle wakeup, or sustained scheduler preemption. A full-rate write of
`cat /etc/os-release` was still corrupted to `sat /etc/os-relee`; reducing
the polling interval to 1 ms does not make burst input reliable. Final
physical reset followed the unchanged default U-Boot path, loaded the vendor
Linux 5.10 image, and returned to its root prompt.

The bounded `/bin/timer_preempt` candidate is QEMU- and physical-board-
validated:

```text
/home/gyy/a20-ls2k-timer-preempt-20260818.bin
size: 2,764,632 bytes
SHA-256: 0201dcdfb501c174e6139d4b2730dd6c27323113afa13a46d2fd3005639b5fe6
QEMU log: /tmp/a20-qemu-la64-timer-preempt-20260818.log
board path: /boot/a20-ls2k-timer-preempt-20260818.bin
board log: /tmp/a20-ls2k-timer-preempt-board-run-20260818.log
```

The test runs one CPU-bound child for about 750 ms and one child that sleeps
for 100 ms. QEMU reported `order=SH first_ms=148 total_ms=755` and `PASS`, so
the sleeper ran before the hog completed. The physical board reported
`order=SH first_ms=156 total_ms=750` and `PASS`, then completed `help`,
`cat /etc/os-release`, `ps`, and `sleep 1; echo SLEEP_OK`. This validates
sustained timer-driven preemption plus the test's child exit/wait path on
hardware. Cooperative LS2K, non-cooperative LS2K, QEMU, and
`check-ls2k1000-build` builds pass. A final physical reset followed the
unchanged default U-Boot path, verified and booted `Linux-5.10.0.lsgd+`, and
returned to the vendor root prompt, so the recovery check passed for this
exact run.

The next RAM-only candidate adds a bounded `/bin/timer_idle` test:

```text
/home/gyy/a20-ls2k-timer-idle-20260818.bin
size: 2,838,392 bytes
SHA-256: bb74dbc71705beb47febe293862b427d864631a24033c62d3e9414e5e9ab17e2
QEMU log: /tmp/a20-qemu-la64-timer-idle-20260818.log
```

It enables and snapshots `/proc/a20/perf`, waits with
`poll(NULL, 0, 200)`, then requires both `idle_wait_entries` and
`idle_wait_wake_returns` to increase. QEMU reported an exact 200 ms timeout,
deltas of 45 for both counters, and `PASS`; the preemption and shell smoke
tests also passed on the same image. On hardware `timer_idle` itself passed
with a 202 ms timeout and deltas of 50, but a following `timer_preempt` hung
after printing `start`. The candidate is rejected because reading the perf
file left global syscall profiling enabled and contaminated the following
stress test.

The corrected candidate restores perf collection to disabled before
`timer_idle` returns:

```text
/home/gyy/a20-ls2k-timer-idle-v2-20260818.bin
size: 2,838,392 bytes
SHA-256: c4fd8c5b221da3b5bb75330a09e486872068efc4de9a65a0ac248518d4d2b6e0
QEMU log: /tmp/a20-qemu-la64-timer-idle-disable-20260818.log
board path: /boot/a20-ls2k-timer-idle-v2-20260818.bin
board log: /tmp/a20-ls2k-timer-idle-v2-board-run-20260818.log
```

QEMU passed `timer_idle` followed immediately by `timer_preempt` and the shell
smoke commands. Cooperative LS2K, non-cooperative LS2K, QEMU, and
`check-ls2k1000-build` builds pass. The physical board then reported a 202 ms
timeout, idle-entry and wake-return deltas of 50, and `TIMER_IDLE: PASS`.
Running `timer_preempt` immediately afterward reported
`order=SH first_ms=156 total_ms=750` and `PASS`, followed by successful `help`,
`cat /etc/os-release`, and `ps`. This validates the focused Phase 1 timeout,
idle-wakeup, sustained-preemption, and exit/wait paths on hardware. A later
combined `sleep` command was corrupted by the polling UART and is not counted
as a result of this run; timed sleep/wakeup was already validated with the
earlier timer-preempt candidate. Physical reset followed the unchanged U-Boot
default path, verified and booted `Linux-5.10.0.lsgd+`, and returned to the
vendor root prompt.

The remote `main` history was subsequently rewritten.  The port was replayed
onto `7523d79e`, and the post-rebase timer/UART baseline was built at
`78f5c2af`:

```text
/home/gyy/a20-ls2k-rebase-uartirq-v2-20260818.bin
board path: /boot/a20-ls2k-rebase-uartirq-v2-20260818.bin
size: 2,842,496 bytes
SHA-256: 7356c48836efeaf0a1343f953b73322bedb2b3cfad3330fab426c7918c68d148
QEMU log: /tmp/a20-rebase-qemu-focus-20260818.log
board log: /tmp/a20-ls2k-rebase-uartirq-v2-board-run2-20260818.log
cooperative fallback: 2,838,400 bytes
cooperative SHA-256: 565c109c3bc6f7c1d7999130705969c8a9d35624eccaa3c7121da9f053358316
QEMU image: 2,826,112 bytes
QEMU SHA-256: 3de6a073777fb66dd06336b16d4321df5813e7b5aa32c44cefbd53c7f330bad7
```

Strict cooperative, non-cooperative, QEMU, and `check-ls2k1000-build`
builds passed.  QEMU passed `timer_idle`, `timer_preempt`, and the shell smoke
commands.  The board passed the same focused tests; a burst containing four
`echo` commands and one `cat` command arrived intact, UART0/cascade counts
rose from 21 to 139, and spurious/storm counts remained zero.  A physical
reset then booted the unchanged vendor `Linux-5.10.0.lsgd+` image and returned
to the root prompt.  Use this artifact and commit as the current post-rebase
physical-board baseline; older hashes remain historical evidence only.

The first Phase 3 no-carrier GMAC candidate was then RAM-booted:

```text
/home/gyy/a20-ls2k-gmac-nolink-v1-20260818.bin
board path: /boot/a20-ls2k-gmac-nolink-v1-20260818.bin
size: 2,842,520 bytes
SHA-256: 9c4606935b2d66d63df4c64d6147c8289c9ca9b0ace399c6a2981678e8314e63
QEMU log: /tmp/a20-qemu-gmac-nolink-v1-20260818.log
board upload log: /tmp/a20-ls2k-gmac-nolink-v1-board-run-20260818.log
board boot/recovery log: /tmp/a20-ls2k-gmac-nolink-v1-ramboot-single-20260818.log
```

The board reported MAC version `0x0000d137`, PHY ID `0x0000010a`, continued
with carrier down, bound one GMAC device, attached lwIP, and reached `mksh`.
`timer_idle`, `timer_preempt`, the shell smoke commands, and the interrupt
status check passed without a GMAC interrupt storm.  DMA and GMAC LIOINTC
interrupts remained disabled.  Physical reset then returned through the
unchanged default U-Boot path to the vendor Linux root prompt.  This validates
only no-cable discovery and link-down integration; descriptor data transfer,
link-up, ping, and socket traffic still require an Ethernet cable.

## What Is Working

- U-Boot can load A20OS at cached DMW address `0x9000000002000000` and enter it
  with `go`.
- The board-specific linker script keeps the kernel inside the verified low
  memory window and aligns the general exception entry to 4 KiB.
- Cached and uncached DMW aliases are installed for RAM and MMIO.
- Memory allocation is restricted to physical `0x00200000..0x0b000000` so the
  kernel does not consume U-Boot, framebuffer, or other firmware-owned RAM.
- LA264 uses a non-folded four-level user page table. Do not replace this with
  the QEMU-folded layout.
- General exceptions and TLB refill reach the kernel correctly on hardware.
- Userspace reaches PLV3 and runs embedded `/bin/init`, `/bin/mksh`, `help`,
  `ls`, `cat`, and `ps` from a writable RAM filesystem.
- The experimental non-cooperative image reaches the shell with local timer
  interrupts enabled, survives repeated timer exception return, and completes
  one- and two-second `/bin/sleep` tests. A bounded CPU-hog/sleeper test also
  confirms sustained timer-driven preemption and child exit/wait on hardware;
  a 200 ms poll test confirms timeout-driven idle entry and wake return. Keep
  the cooperative image as the recovery baseline while later board subsystems
  are brought up.
- The non-cooperative image routes LIOINTC UART0 source 0 to CPU0 HWI1. Burst
  serial input, repeated delivery, mask/ack/re-enable, bounded dispatch, and
  coexistence with the local timer are physical-board-validated. The
  cooperative image retains polling as its recovery-console fallback.
- Cooperative scheduling keeps interrupt delivery disabled during the current
  shell bring-up and avoids the previously observed syscall-entry deadlock.
- Dense trap/syscall UART markers are disabled by default and only compile
  when `CONFIG_LS2K_TRAP_TRACE` is explicitly defined.
- Physical-board behavior is selected with `CONFIG_BOARD_LS2K1000`; the
  cooperative exceptions additionally require `CONFIG_COOPERATIVE_BOOT`.
- A separate `STORAGE_READ_ONLY=1` profile implements the LS2K1000 AHCI
  platform front end, polling IDENTIFY/read commands, MBR type-`0x83`
  partition wrapping, and ext4 read-only mounting. This polling path is
  physical-board-validated and must not be folded into the cooperative fallback.

## Known Limitations

- Only `NR_CPUS=1` is supported on the physical board. Secondary-core startup,
  IPI delivery, and TLB shootdown are not validated.
- Timer interrupts remain disabled in the known-good cooperative profile.
  Basic local timer delivery is physical-board-validated in a separate
  experiment; timed sleep/wakeup, sustained preemption, timeout handling, and
  idle wakeup pass focused single-core tests. Broader scheduler stress and SMP
  timer behavior remain unvalidated.
- LIOINTC support is limited to UART0 source 0 on CPU0. All other device
  sources remain masked; GMAC, AHCI, USB, GPIO, and other board devices must
  not depend on IRQ delivery yet.
- UART RX is interrupt-driven only in the non-cooperative experiment. The
  cooperative recovery profile still polls at 1 ms and can lose characters
  on burst input, so commands to that fallback image must remain rate-limited.
- GMAC0 is DWMAC1000 Synopsys `0x37`, RGMII, with a Motorcomm PHY at address
  0 and ID `0x0000010a`. The Phase 3 candidate treats missing carrier as
  non-fatal and exposes link state to lwIP; this no-cable discovery path is
  physical-board-validated. Link-up and all packet data transfer remain
  unvalidated. The cooperative recovery profile skips GMAC enumeration.
- Normal and cooperative bring-up modes do not mount block devices. The
  working fallback shell uses embedded programs and RAMFS only. Only the
  explicit `STORAGE_READ_ONLY=1` experiment scans SATA and attempts `/test`.
- The physical board does not expose QEMU's ECAM at `0x20000000`. A Loongson
  PCI configuration-window implementation is required before enabling normal
  PCI enumeration.
- AHCI/SATA polling reads are QEMU- and physical-board-validated with MBR/ext4.
  QEMU additionally covers reads across 4 GiB. AHCI source 19 remains masked,
  all writes are blocked, and ext4 `needs_recovery` causes mount refusal
  without journal replay.
- Board poweroff and reboot fall back to halting the CPU.
- Some scheduler and page-table diagnostics are intentionally still verbose.
  Remove them only after the corresponding hardware path is stable and the
  resulting image has passed both board and QEMU regression tests.

## Non-Negotiable Recovery Rules

Never execute any of the following while developing this port:

```text
saveenv
sf write
sf erase
```

Also:

- Never overwrite `/boot/uImage`.
- Upload experimental kernels under a new filename.
- Use `sf probe` and `sf read` only for the existing DTB read path.
- Do not change U-Boot's persistent `bootcmd` or environment.
- Do not attempt to jump back into U-Boot while A20OS is running.
- Recover from an A20OS hang with a physical reset. The unchanged default
  boot path must return to vendor Linux.

Recovery material already captured for this board:

- Board copy: `/root/a20-recovery-20260817/`
- WSL archive: `/home/gyy/a20-board-recovery/a20-recovery-20260817.tar.gz`
- Archive SHA-256:
  `2c4aa17a39c550b8edf1acca85b3a198da5f10cbec9f4d8cc1c100db11159088`

Before any future work that could affect persistent media, stop and obtain
explicit user approval even if a recovery archive exists.

## Architecture Boundaries

- Keep board addresses, RAM windows, clocks, and IRQ routing under
  `kernel/platform/ls2k1000/` or in board configuration data.
- Do not add LS2K1000 addresses to generic LoongArch code.
- Drivers must consume resources supplied by the board and must not branch on
  `CONFIG_BOARD_LS2K1000`.
- Keep QEMU and physical-board exception/MMU behavior separate with narrow
  compile-time guards. Do not make LS2K1000 workarounds the LoongArch default.
- Preserve QEMU's folded page-table behavior and LS2K1000's non-folded Dir2
  behavior as separate MMU layouts.
- Do not re-enable QEMU PCH-PIC/EIOINTC access on LS2K1000.
- Do not call `pci_enumerate()` on LS2K1000 until a real Loongson PCI host
  implementation exists.
- Use structured DTB parsing and verified vendor/Linux register definitions;
  do not infer device register layouts from serial symptoms alone.

## Build Commands

Known-good physical-board RAM-shell profile:

```sh
make -j"$(getconf _NPROCESSORS_ONLN)" \
  ARCH=loongarch64 BOARD=ls2k1000 ABI=linux BRINGUP=1 \
  RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded COOPERATIVE_BOOT=1 \
  kernel-only
```

Expected binary:

```text
.kernel-build/loongarch64-ls2k1000-linux-bringup-ramfs-user-embedded-cooperative/kernel.bin
```

Read-only SATA experiment (never combine with `COOPERATIVE_BOOT=1`):

```sh
make -j"$(getconf _NPROCESSORS_ONLN)" \
  ARCH=loongarch64 BOARD=ls2k1000 ABI=linux BRINGUP=1 \
  RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded STORAGE_READ_ONLY=1 \
  kernel-only
```

Expected binary:

```text
.kernel-build/loongarch64-ls2k1000-linux-bringup-ramfs-user-embedded-storage-ro/kernel.bin
```

QEMU regression profile matching the embedded RAM shell:

```sh
make -j"$(getconf _NPROCESSORS_ONLN)" \
  ARCH=loongarch64 BOARD=qemu-virt-loongarch64 ABI=linux BRINGUP=1 \
  RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded kernel-only
```

Run it with:

```sh
qemu-system-loongarch64 -machine virt -m 1G -nographic -smp 1 \
  -kernel .kernel-build/loongarch64-qemu-virt-loongarch64-linux-bringup-ramfs-user-embedded/kernel.elf
```

Also run the repository build gate after relevant shared changes:

```sh
make check-ls2k1000-build
```

Do not assume a successful build proves hardware behavior.

## RAM-Only Boot Procedure

Verify the uploaded file hash in vendor Linux, reboot, stop autoboot with the
lowercase `c` key (or `tools/ls2k1000-uboot-stop.runscript`), and run commands
one at a time:

```text
sf probe
sf read ${fdt_addr} dtb
scsi reset
ext4load scsi 0:1 0x9000000002000000 /boot/<new-image-name>.bin
go 0x9000000002000000
```

Do not paste write commands or command sequences containing persistent U-Boot
operations. Capture every run with a new minicom log under `/tmp`.

## Required Validation After Each Board Change

1. Build the cooperative LS2K1000 profile and record image size and SHA-256.
2. Rebuild the QEMU profile.
3. Boot QEMU to `mksh` and run `help`, `cat /etc/os-release`, and `ps`.
4. For changes intended only for LS2K1000, inspect the QEMU preprocessor path
   or relevant object hashes to confirm the board code compiled out.
5. Upload under a new `/boot` filename and verify its SHA-256 on vendor Linux.
6. RAM-boot the board and test only the behavior changed by the patch, followed
   by the three shell smoke commands.
7. Physically reset once and confirm vendor Linux still boots normally.
8. Update `docs/platforms/physical-boards.md` with newly verified facts and
   clearly label unverified assumptions.

Do not run the complete syscall suite for a board-device change. The shell
smoke commands already exercise the critical process/syscall path; add focused
tests proportional to the subsystem being changed.

## Next Work, in Order

### Phase 1: Local Timer and Preemptive Scheduling

- Preserve the cooperative image as the fallback baseline.
- Build a separate non-cooperative experiment; do not overwrite the fallback
  board image.
- Basic LoongArch timer CSR programming, ECFG local timer enable,
  acknowledge/rearm ordering, and repeated exception return have reached the
  physical-board shell. The shell smoke commands and timed `/bin/sleep` have
  passed with controlled-rate serial input. The bounded `/bin/timer_preempt`
  test has also validated sustained preemption and child exit/wait on board.
- The corrected `/bin/timer_idle` candidate directly measures entry to and
  return from the architecture idle wait around a 200 ms poll timeout. It
  passed on QEMU and the physical board, and a following `timer_preempt` run
  confirms that the test restores global profiling state before returning.
- Phase 1's focused single-core acceptance points are complete. Keep the
  cooperative image as the fallback and do not treat this as SMP or broad
  scheduler-stress validation.

### Phase 2: LS2K1000 Device Interrupt Controller

- Vendor DTB, live-register, and matching driver evidence is recorded in
  `docs/platforms/2k1000.md`; it supersedes conflicting manual addresses.
- The board/SoC controller is isolated from QEMU EIOINTC and currently enables
  only UART0 source 0. Cooperative polling remains available as a fallback.
- UART0 repeated delivery, masking, acknowledgement/re-enable, burst input,
  and timer coexistence passed on hardware with no spurious or storm count.
- Next acceptance point: validate one additional low-risk source, including
  trigger type, mask/ack behavior, and bounded failure handling, before any
  GMAC or storage interrupt path is enabled.

### Phase 3: GMAC Data Path

- Vendor DTB/Linux inspection confirms DWMAC1000 Synopsys `0x37`, RGMII,
  PHY address 0, and Motorcomm PHY ID `0x0000010a`. No explicit DT delay
  properties are present; Motorcomm extended-register setup still needs audit.
- The polling candidate no longer fails probe when carrier is absent. It uses
  basic ring descriptors, corrects ring/segment bits, synchronizes descriptors
  before ownership checks, and leaves device IRQs disabled.
- The no-cable acceptance point is complete: the board produced a single
  successful GMAC probe with carrier down, reached `mksh`, and passed the
  shell/timer smoke tests without enabling GMAC interrupts.
- Next acceptance point with a cable: validate PHY link parameters, then
  exercise descriptor ownership and cache maintenance with repeated traffic.
- Acceptance point: stable link plus repeated ping and socket traffic, first
  polled and then interrupt-driven.

### Phase 4: PCI Configuration and AHCI/SATA

- Implement the Loongson PCI configuration-window host access shim.
- Enumerate only verified devices and BARs; never reuse QEMU ECAM constants.
- The onboard AHCI controller does not require PCI enumeration for the first
  experiment. Its verified physical MMIO window is supplied by an LS2K1000
  platform device; the shared driver remains free of board addresses.
- Polling AHCI reads, aligned DMA structures, MBR `0x83` partition wrapping,
  ext4 read-only mounting, `EROFS` propagation, and a bounded-memory large-file
  reader are implemented. QEMU validates clean mounts, dirty-filesystem
  refusal, full reads, 4 GiB boundary reads, and unchanged backing images.
- The RAM-only board candidate passed IDENTIFY, capacity and MBR checks, mounted
  ext4 read-only, completed `storage_read_test ... sample` and shell smoke, then
  recovered to vendor Linux after physical reset. Keep source 19 masked and do
  not write or replay the vendor filesystem journal.
- The Loongson PCI host window remains separate future work for PCI devices.
  Enable storage writes only after explicit user approval and dedicated
  recovery planning.

### Phase 5: SMP

- Add secondary-core startup, per-CPU state, IPI routing, and TLB shootdown.
- Keep the production board build at `NR_CPUS=1` until stress tests pass.
- Acceptance point: both cores online under scheduler, fork/exit stress, and
  cross-CPU TLB invalidation without corruption.

### Phase 6: Boot Integration

- Consider a non-default U-Boot menu entry only after timer, storage, and
  recovery tests are stable.
- Do not replace vendor Linux, overwrite `/boot/uImage`, or persist a new
  default boot command as part of normal porting work.

## Evidence and Handoff

For every physical-board experiment, record:

- commit ID and exact build command;
- image filename, byte size, and SHA-256 on both WSL and board;
- firmware and board revision;
- full serial log path;
- commands run and observed result;
- whether physical reset returned to vendor Linux;
- QEMU regression result.

Do not report a subsystem as supported from build success alone. Use the terms
`implemented`, `QEMU-validated`, and `physical-board-validated` precisely.
