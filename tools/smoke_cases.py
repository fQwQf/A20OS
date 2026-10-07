"""Smoke/gate case table -- source of truth for tools/smoke.py.

Originally migrated from the pre-migration inline make recipes (every argv, log
path, timeout, pass pattern, forbidden pattern and timeout flag was verified
against make's own expansion at migration time).  The pinned-revision extractor
that generated it is gone; edit this table directly and keep it in sync with
the .mk files it replaced -- tools/smoke_audit.py (check-smoke-cases) fails when
a case and a make target disagree.

Semantics worth knowing when editing a case:
  expect      every pattern must match the log (grep -q, i.e. a regex)
  forbid      no pattern may match -- e.g. smoke-socket-stress passes only
              when SOCKET_STRESS: PASS is present AND no [LOCK] line is
  timeout_msg distinguish exit 124 (the QEMU timeout) in the failure message
"""

from __future__ import annotations

CASES: dict[str, dict] = {
    'smoke-a20-channel': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/a20-channel-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['a20_channel_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['A20_CHANNEL: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-a20-channel: PASS; log saved to $log',
    },
    'smoke-audio-userspace': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['rm -f user/build/x86_64/hda.a20drv', 'rm -f /tmp/a20-smoke-audio.wav'],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/audio-userspace-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/audioplay --tone 440 --duration 5000', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-snapshot', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-audiodev', 'driver=wav,id=audio0,path=/tmp/a20-smoke-audio.wav', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/kernel.elf'],
        'expect': ['audioplay: 440 Hz for 5000 ms -> /dev/audio', 'audioplay: playback complete', '\\[HDA\\] playback starts=1 underruns=0', 'System is going down for power-off NOW'],
        'forbid': ['audioplay: playback failed'],
        'post': ['python3 tools/check_wav_pcm.py --min-frames 8000 /tmp/a20-smoke-audio.wav'],
        'timeout_msg': False,
        'pass_msg': 'smoke-audio-userspace: PASS; log=$log wav=/tmp/a20-smoke-audio.wav',
    },
    'smoke-clock-vdso': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/clock-vdso-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/clock_bench', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['CLOCK_BENCH: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-clock-vdso: PASS; log saved to $log',
    },
    'smoke-coredump': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/coredump-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['coredump_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['COREDUMP_TEST: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-coredump: PASS; log saved to $log',
    },
    'smoke-driver-lifecycle': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=1', 'CONFIG_DRIVER_LIFECYCLE_TEST=y'], 'target': 'kernel-only'},
        'log': '.kernel-build/smoke/driver-lifecycle-riscv64.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-driver-lifecycle/kernel.elf'],
        'expect': ['DRIVER_LIFECYCLE: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-driver-lifecycle: PASS; log saved to $log',
    },
    # Platform-bus IRQ resource channel.  QEMU has no dw-mshc model, so this
    # covers the half that is pure kernel code -- resource -> platform device
    # -> request_irq -> simulated dispatch -> free_irq, plus the -ENODEV
    # fallback and the driver's a20.dw-sdio.poll decision -- and NOT the
    # controller's own interrupt generation, which remains unverified.
    'smoke-platform-irq-fallback': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=1', 'CONFIG_PLATFORM_IRQ_TEST=y'], 'target': 'kernel-only'},
        'log': '.kernel-build/smoke/platform-irq-fallback-riscv64.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-platform-irq/kernel.elf'],
        'expect': ['PLATFORM_IRQ_TEST: PASS'],
        'forbid': ['PLATFORM_IRQ_TEST: FAIL'],
        'timeout_msg': True,
        'pass_msg': 'smoke-platform-irq-fallback: PASS; log saved to $log',
    },
    'smoke-drvmod-aarch64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=aarch64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/drvmod-aarch64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-aarch64',
        'argv': ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a57', '-m', '1G', '-nographic', '-smp', '1', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/aarch64-qemu-virt-aarch64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-kernel', '.kernel-build/aarch64-qemu-virt-aarch64-both-dev/kernel.elf'],
        'expect': ['\\[GOLDFISH-RTC\\] probe ok', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-drvmod-aarch64: PASS (rtc.a20drv loaded and bound); log saved to $log',
    },
    'smoke-drvmod-loongarch64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=loongarch64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/drvmod-loongarch64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-loongarch64',
        'argv': ['qemu-system-loongarch64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/kernel.elf'],
        'expect': ['\\[GOLDFISH-RTC\\] probe ok', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-drvmod-loongarch64: PASS (rtc.a20drv loaded and bound); log saved to $log',
    },
    'smoke-drvmod-riscv64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/drvmod-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['drvctl list', 'syscall_smoke', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['\\[GOLDFISH-RTC\\] probe ok', 'SYSCALL_SMOKE: PASS', 'rtc.a20drv',
                   'abi-probe\\.a20drv: invalid descriptor',
                   'System is going down for power-off'],
        'forbid': ['abi-probe: LOADED'],
        'timeout_msg': False,
        'pass_msg': 'smoke-drvmod-riscv64: PASS (rtc.a20drv loaded and bound; abi-probe.a20drv refused on ABI mismatch, DriverEntry never ran); log saved to $log',
    },
    'smoke-drvmod-x86_64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/drvmod-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['drvctl list', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': ['\\[PC-SPKR\\] driver registered in core: 0', '\\[PS2\\] module init ok'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-drvmod-x86_64: PASS (pc-spkr.a20drv + ps2.a20drv loaded, registered, bound); log saved to $log',
    },
    'smoke-dual-input': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/dual-input-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-device', 'virtio-keyboard-device,bus=virtio-mmio-bus.5', '-monitor', 'unix:$monsock,server,nowait', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['UINPUT] kernel-placement probe: id=18 version=2 name=QEMU Virtio Keyboard', 'UINPUTD: name=QEMU Virtio Keyboard', 'UINPUTD: ready', 'UINPUTD: ev type=1 code=30 value=1', 'UINPUTD: claimed', 'UINPUTD: PASS', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-dual-input: PASS; log saved to $log',
    },
    'smoke-envelope': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/envelope-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['envelope_smoke', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['ENVELOPE_SMOKE: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-envelope: PASS; log saved to $log',
    },
    'smoke-envelope-bench': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/envelope-bench-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['envelope_bench', 'poweroff']},
        'timeout': '180s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['ENVELOPE_BENCH: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-envelope-bench: PASS; log saved to $log',
    },
    'smoke-envelope-corpus': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/envelope-corpus-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['envelope_corpus', 'poweroff']},
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['ENVELOPE_CORPUS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-envelope-corpus: PASS; log saved to $log',
    },
    'smoke-envelope-pilot': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/envelope-pilot-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['envelope_pilot', 'poweroff']},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['ENVELOPE_PILOT: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-envelope-pilot: PASS; log saved to $log',
    },
    'smoke-evdev-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/evdev-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['evdev_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['EVDEV_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-evdev-stress: PASS; log saved to $log',
    },
    'smoke-futex-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/futex-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['futex_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['FUTEX_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-futex-stress: PASS; log saved to $log',
    },
    'smoke-futex-stress-aarch64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=aarch64', 'BOARD=qemu-virt-aarch64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/futex-stress-aarch64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['futex_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-aarch64',
        'argv': ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a57', '-m', '1G', '-nographic', '-smp', '1', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/kernel.elf'],
        'expect': ['FUTEX_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-futex-stress-aarch64: PASS; log saved to $log',
    },
    'smoke-hda': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['rm -f user/build/x86_64/hda.a20drv'],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=both', 'BRINGUP=0', 'CONFIG_HDA_SMOKE_TEST=y', 'DRVMOD_SMOKE=1'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hda-x86_64.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-audiodev', 'driver=none,id=audio0', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio0', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev-hda-smoke/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev-hda-smoke/kernel.elf'],
        'expect': ['HDA_STREAM_SMOKE: PASS', '\\[HDA\\] driver registered in core: 0'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-hda: PASS; log saved to $log',
    },
    # virtio-rng over PCI: the driver binds virtio-rng-pci, publishes a CHAR
    # class device named /dev/hwrng, and hwrng_test reads >= 256 bytes out of
    # it.  The three shape checks live in the test program (not all 0x00, not
    # all 0xff, not one repeated byte) so the kernel log only has to carry the
    # driver's own readiness line and the test verdict.
    'smoke-virtio-rng': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/virtio-rng-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['hwrng_test', 'poweroff']},
        'timeout': '30s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-device', 'virtio-rng-pci', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': ['\\[VRNG\\] virtio-rng ready',
                   'HWRNG_TEST: PASS',
                   'System is going down for power-off'],
        'forbid': ['PANIC|Kernel panic|virtio-rng.*unresolved symbol', 'HWRNG_TEST: FAIL'],
        'timeout_msg': True,
        'pass_msg': 'smoke-virtio-rng: PASS (virtio-rng.a20drv bound over PCI, /dev/hwrng returned entropy); log saved to $log',
    },
    'smoke-io-event': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/io-event-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['io_event_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=@BUILD_DIR@/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '@BUILD_DIR@/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
        'expect': ['IO_EVENT_TEST: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-io-event: PASS; log saved to $log',
    },
    'smoke-iommu-udriver-isolation': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0', 'PROFILE=benchmark'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/iommu-udriver-isolation-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['cat /proc/a20/iommu', 'poweroff']},
        'timeout': '30s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-device', 'riscv-iommu-pci,bus=pcie.0,addr=1.0', '-device', 'edu,bus=pcie.0,addr=2.0,dma_mask=0xffffffffffffffff', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['UEDUD: mapped DMA ok', 'UEDUD: unmapped DMA fault', '\\[IOMMU\\] DMA fault blocked did=16 cause=15', 'UEDUD: recovered', 'UEDUD: PASS', 'enabled: 1', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-iommu-udriver-isolation: PASS; log saved to $log',
    },
    'smoke-mlibc': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mlibc-riscv64.log',
        'post-build': ['make mlibc-rootfs'],
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/mlibc-hello-rv', '/bin/mlibc-pipeexec-rv', '/bin/mlibc-pipeexec-rv', 'poweroff']},
        'timeout': '40s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['MLIBC_A20: PASS', 'PIPEEXEC: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mlibc: PASS; log saved to $log',
    },
    'smoke-mlibc-fork': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mlibc-fork-riscv64.log',
        'post-build': ['make mlibc-rootfs'],
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/mlibc-fork-rv', 'poweroff']},
        'timeout': '40s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['MLIBC_FORK: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mlibc-fork: PASS; log saved to $log',
    },
    'smoke-mlibc-mksh': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mlibc-mksh-riscv64.log',
        'post-build': ['make mlibc-rootfs'],
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/mlibc-mksh /bin/test-mlibc-mksh.sh', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['MKSH_MLIBC: PASS', 'MKSH_MLIBC: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mlibc-mksh: PASS; log saved to $log',
    },
    'smoke-mlibc-sbase': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mlibc-sbase-riscv64.log',
        'post-build': ['make mlibc-sbase-rootfs'],
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/test-mlibc-sbase.sh', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['MLIBC_SBASE: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mlibc-sbase: PASS; log saved to $log',
    },
    'smoke-mm-fork-exec-race': {
        'gate': {'mem': '1G', 'cpus': '8'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0', 'NR_CPUS=8'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mm-fork-exec-race-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['mm_stress --vma-fork-exec-only', 'poweroff']},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '8', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/kernel.elf'],
        'expect': ['MM_VMA_FORK_EXEC: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mm-fork-exec-race: PASS; log saved to $log',
    },
    # Keep the RV64 kernel-trap t0 frame slot covered by a real timer interrupt.
    'smoke-rv64-trap-t0': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/rv64-trap-t0.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-preempt/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-preempt/kernel.elf', '-append', 'a20.trap_t0_selftest=1'],
        'expect': ['RV64_TRAP_T0: PASS', 'System is going down for power-off NOW'],
        'forbid': ['RV64_TRAP_T0: FAIL', 'KERNEL PANIC|Kernel panic|PANIC'],
        'timeout_msg': False,
        'pass_msg': 'smoke-rv64-trap-t0: PASS (real timer IRQ preserved kernel t0); log saved to $log',
    },
    # Page-table cursor race gate.
    #
    # anonprov=4096 is load-bearing, not decoration: mm_pt_provision_anon() is
    # the only source of a cursor whose covering level is > 0, and it is off by
    # default.  Drop the -append and no wide cursor is ever created, so the
    # wide-vs-narrow leaf collision never occurs and this gate silently stops
    # testing anything.  SMP=8 lets those cursors be in flight on several CPUs.
    #
    # A PASS is NOT evidence that the cursor leaf lock is race-free.  Both
    # mm_pt_provision_anon() and the fault path still hold mm->lock, so they
    # serialise and cannot collide yet; this is a hang/crash/audit-drift gate
    # until the fault path leaves mm->lock, and only then a race gate.  The
    # covering-node-only design satisfied every presence-style gate while being
    # wrong for exactly this reason.
    #
    # The MM-ASM line, not MM_STRESS: PASS, is the evidence: it is the
    # bidirectional audit of per-PTE status against the hardware page tables
    # over every live address space, and all-zero means the two representations
    # never diverged.
    'smoke-mm-pt-race': {
        'gate': {'mem': '1G', 'cpus': '8'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0', 'NR_CPUS=8'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mm-pt-race-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  # The leading perf read is what ARMS collection, not a
                  # measurement: a20_perf_format() sets g_a20_perf_enabled and
                  # only then snapshots, so counters read dormant read 0 no
                  # matter what the workload did.  Reading once after the
                  # workload therefore reports zeros for everything -- which is
                  # exactly how this case came to look like dead code.
                  'lines': ['cat /proc/a20/perf', 'mm_stress --wide-cursor-only',
                            'cat /proc/a20/anonprov', 'cat /proc/a20/perf',
                            'poweroff']},
        'timeout': '240s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '8', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/kernel.elf', '-append', 'a20.anonprov=4096'],
        'expect': [
            # --wide-cursor-only exits after this phase, so MM_STRESS: PASS is
            # not printed and must not be expected here.
            'MM_WIDE_CURSOR: PASS',
            # Matched by field name, not by position: the seg counters are printed after
            # anon_virt, so the adjacency of the fields before them is not
            # something this gate may assume.  It did once, and an all-zero
            # audit line failed the gate.
            r'\[MM-ASM\].*missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 vmai=0 cls=0 safe=0.*\bseg_bad=0\b.*\bseg_kind=0\b.*\bseg_diff=0\b',
            # Non-vacuity for the status fast path, asserted rather than assumed.
            # This gate is the only thing standing between "green" and "the fast
            # path never ran": the workload exits without printing fault counters,
            # and without anonprov a provisioned range is never consumed, so the
            # phase this item is about would be dead code and still pass.  Same
            # trap that made smoke-mm-stress pass while testing nothing.
            r'mm_fault_from_status: [1-9]',
        ],
        # The per-CPU pool overflow and the self-deadlock detector in
        # mcs_lock() both announce themselves; either firing means the cursor
        # lock discipline is unbalanced.
        'forbid': ['MCS DEADLOCK', 'already_holding', 'page-table lock nesting exceeded'],
        'timeout_msg': False,
        'pass_msg': 'smoke-mm-pt-race: PASS; log saved to $log',
    },
    # The ordered-index capacity-overflow fallback in mm_seg_find().
    #
    # This gate exists because the branch was both untested and wrong: the
    # rebuild was retried on every lookup while the state said "over capacity",
    # so every fault in an address space past MM_SEG_INDEX_CAPACITY paid a full
    # 1024-entry rebuild and then the list walk it was meant to replace.  No
    # workload in the tree could reach the cap, so nothing noticed.
    #
    # The non-zero assertion is the point of the gate, not decoration.  Asserting
    # only MM_SEG_INDEX_OVERFLOW: PASS would pass just as happily if the
    # workload's mappings had all merged into a handful of records and the
    # overflow branch never ran -- which is what happens unless the workload
    # leaves an unmapped page between each pair, since vma_can_merge()
    # coalesces adjacent equal anonymous mappings.  A green line here has to
    # mean the branch ran.
    'smoke-mm-seg-index-overflow': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mm-seg-index-overflow-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['mm_stress --seg-index-overflow-only', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            'MM_SEG_INDEX_OVERFLOW: PASS',
            # Non-vacuity: the overflow branch really executed.
            r'seg index overflow: [1-9]',
            # And the audit still passes while over the cap -- the list-walk
            # fallback still has to agree with the page tables about every
            # mapping, or "it returned something" would be the only claim.
            r'\[MM-ASM\].*missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 vmai=0 cls=0 safe=0.*\bseg_bad=0\b.*\bseg_kind=0\b.*\bseg_diff=0\b',
        ],
        # The list walk carries a cycle detector that panics; a matching
        # address space must never reach it.
        'forbid': ['VMAWALK'],
        'timeout_msg': False,
        'pass_msg': 'smoke-mm-seg-index-overflow: PASS; log saved to $log',
    },
    'smoke-mm-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mm-stress-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['mm_stress', 'poweroff']},
        # 45s was enough before the huge-leaf path ran: huge_install>0 and
        # cow_from_status>0 mean the phases now do the 2 MiB copies and the
        # lockless COW work they always claimed to, which costs time.
        'timeout': '90s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            'MM_STRESS: PASS',
            # mm_stress runs the huge-page phases (basic / fork-COW / prot),
            # the only workload in the gate set that exercises pt_map_huge
            # and the huge-leaf demote path.  That is exactly why the audit
            # line must be asserted *here*: before the huge leaf carried
            # status, every THP fault left a present_mismatch that only
            # smoke-mm-pt-race and smoke-mm-software would have caught --
            # and neither of those maps huge pages, so the gap was invisible
            # to every gate that could have seen it.
            r'\[MM-ASM\].*missing_meta=0 present=0 absent=0 prot=0 cow=0 vma=0 vmai=0 cls=0 safe=0.*\bseg_bad=0\b.*\bseg_kind=0\b.*\bseg_diff=0\b',
            # Non-vacuity for the huge-leaf path, same discipline as the
            # mm_fault_from_status assertion in smoke-mm-pt-race: huge_install=0
            # means the workload never had a huge leaf and every huge-path
            # assertion above proved nothing.  Plain globals printed in
            # [MM-ASM]; matched WITHOUT the line prefix because a concurrent
            # klog line can split the audit record mid-print, and both names
            # exist nowhere else in the kernel's output.
            r'huge_install=[1-9]',
            # Non-vacuity for the lockless COW slice: mm_stress's fork phases
            # make shared anonymous COW leaves, and mm_cow_from_status must
            # be the one serving them (rc>1 copies).  cow_from_status=0 with
            # the fork phases green means the fast path declined every fault
            # and the mm->lock path did all the work again.
            r'cow_from_status=[1-9]',
        ],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mm-stress: PASS; log saved to $log',
    },
    'smoke-hyp-selftest': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-selftest-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['cat /proc/a20/hyp_selftest', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # The per-step lines go to the console; the read's one-line
            # verdict is what a shell can gate on.  Reading the file RUNS
            # the selftest (create -> map 4 pages -> translate/readback ->
            # lend-flag -> s2 audit -> unmap/remap/-EEXIST -> destroy).
            # -cpu rv64,h=true: the default rv64 CPU does not expose the H
            # extension, and hyp_supported() then makes the file SKIP --
            # which would make this gate pass while testing nothing.
            'HYP_SELFTEST: PASS',
            'hyp_selftest=PASS',
        ],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-selftest: PASS; log saved to $log',
    },
    # QEMU still exposes hstatus on an H-less CPU, so CSR probing alone lets
    # this selftest pass into a later illegal-instruction panic.  The FDT ISA
    # gate must reject H before touching H CSRs and make the selftest skip.
    'smoke-hyp-no-h': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-no-h-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['cat /proc/a20/hyp_selftest', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=false', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-preempt/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-preempt/kernel.elf'],
        'expect': [
            'HYP_SELFTEST: SKIP (no virtualization extension)',
            'hyp_selftest=PASS',
        ],
        'forbid': ['PANIC', 'Kernel Illegal Instruction', 'HYP_SELFTEST: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-no-h: PASS; log saved to $log',
    },
    # End-to-end vcpu slice: a user program creates a VM, loads 56 bytes of
    # RISC-V machine code into it, runs the guest and asserts the guest left
    # through the SBI shutdown call.  Runs in the same ABI=linux image as the
    # selftest case, through the Linux bridge syscalls, because a Linux-ABI
    # task has no Native handle table to name a VM with.
    'smoke-hyp-vcpu': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-vcpu-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['hyp_test', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        # Same argv as smoke-hyp-selftest, -cpu rv64,h=true included: the
        # default rv64 CPU does not expose the H extension, hyp_supported()
        # then refuses every call, and the gate would go red on a kernel that
        # is merely correct -- or, worse, "pass" against nothing.  The flag is
        # load-bearing, exactly as it is for the selftest.
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            'HYP_VCPU_TEST: PASS',
            # The guest's own console output, as its own line.  The PASS line
            # above only proves the syscall returned; this proves the legacy
            # SBI console_putchar path really executed inside the guest.
            # Anchored, because a bare 'HYP' substring is already contained in
            # 'HYP_VCPU_TEST' and would match vacuously.
            r'^HYP$',
        ],
        'forbid': ['PANIC', 'LOCK-STALL', 'MCS DEADLOCK', 'HYP_VCPU_TEST: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-vcpu: PASS; log saved to $log',
    },
    # Host keystrokes -> guest UART -> host console, the P0 round trip.  This
    # is the independent acceptance point for the guest console input channel
    # (kernel/hyp/hyp_dev.c, hyp_dev_pump_rx): the guest polls the modelled
    # 16550's LSR for DR, pops RBR, and writes the byte back out through SBI
    # console_putchar.  Three bytes in, three bytes out, with only the device
    # model in between.
    #
    # WHY IT NEEDS sendline_seq AND NOT sendline.  The gate's whole subject is
    # the ORDER: the bytes have to be lying in the host rx ring BEFORE the
    # guest gets a chance to poll for them, or the guest just keeps polling.
    # sendline waits for one marker and then dumps every line at once, which
    # happens to work here only because the first marker is the shell prompt
    # and the shell buffers the rest -- but the second step depends on
    # hyp_test having reached its guest, which sendline cannot express at all.
    # Each marker therefore gates its own line: prompt -> start hyp_test,
    # "guest up" -> send the three bytes, hyp_test's own PASS line -> power off.
    #
    # The LAST STEP IS KEYED ON 'HYP_VCPU_TEST: PASS', NOT ON THE SHELL PROMPT,
    # and that is load-bearing rather than cosmetic.  hyp_test destroys the VM
    # before it prints PASS, and hyp_dev_pump_rx() only runs while
    # hyp_vcpu_run() is inside the guest -- so a poweroff typed after that line
    # cannot be stolen into a ring that no longer exists.  Keyed on the prompt
    # instead, the run ended with `# oweroff` / `E: mksh: oweroff: inaccessible
    # or not found` (measured, hyp-console-p0-riscv64.log before this change)
    # and QEMU was left to be killed by the timeout.
    #
    # The prompt marker for the FIRST step is anchored with a preceding newline
    # because the boot banner contains `# ` inside its ASCII art.
    #
    # `rx_bytes=` is the count the device model took on the way IN, which is a
    # separate claim from the echoed bytes: the echo proves they came back out,
    # the counter proves they went through the ring rather than some other
    # path.  Comment the pump out and this case goes red on both.
    'smoke-hyp-console-p0': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-console-p0-riscv64.log',
        'stdin': {'kind': 'sendline_seq', 'steps': [
            ('\n# ', 'hyp_test echo'),
            ('HYP_VCPU_TEST: guest up', 'HYP'),
            ('HYP_VCPU_TEST: PASS', 'poweroff'),
        ]},
        # 120s, not 60s: this is smoke-hyp-vcpu plus a guest that spins in
        # second-stage faults while it waits for the bytes, and a host that is
        # slow to reach the prompt should not be able to fail the gate.
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        # Same argv as smoke-hyp-vcpu, -cpu rv64,h=true included: without the H
        # extension hyp_supported() refuses every call and this gate would go
        # green against nothing.
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # The three stolen bytes, echoed by the guest through the host's
            # SBI handler, as their own line.  Anchored: a bare 'HYP' is
            # already inside 'HYP_VCPU_TEST' and would match vacuously.  The
            # shell's own echo of the typed command cannot produce this line --
            # it is blocked inside hyp_test while the bytes are consumed.
            r'^HYP$',
            'HYP_VCPU_TEST: PASS',
            r'rx_bytes=[1-9][0-9]*',
            # Clean shutdown, not a timeout kill.  smoke.py's report() judges
            # the log text and deliberately does not look at QEMU's exit
            # status, so without this the gate stays green on a run whose only
            # ending was the harness SIGTERM'ing a machine that never powered
            # off.  The same string is what 50-odd other gates assert, so it
            # is not a hypervisor-specific invention.
            'System is going down for power-off NOW',
        ],
        'forbid': ['PANIC', 'LOCK-STALL', 'MCS DEADLOCK',
                   'HYP_VCPU_TEST: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-console-p0: PASS; log saved to $log',
    },
    # A20OS as a guest: /hyp_boot reads the kernel ELF off the image
    # (/boot/guest-kernel.elf), lays its PT_LOAD segments into guest RAM at
    # their link-time physical addresses, hands the vcpu a minimal FDT and runs
    # it.  PASS is the guest's own banner counted by the device model
    # (marker_seen), not a parse of the interleaved console: host and guest
    # share one UART, and only the marker counter separates them.
    #
    # forbid deliberately does NOT list PANIC.  A guest with no block device
    # cannot mount a rootfs and panics in init_kthread ("init: no init program
    # found", kernel/main.c) -- an expected arrival, not a defect -- and its
    # panic text is byte-identical to the host's, so a PANIC forbid would go red
    # on a correct run.  A host that actually dies is caught by the missing
    # 'HYP_A20OS: PASS' expectation instead, which the verdict line cannot
    # produce.
    'smoke-hyp-a20os': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-a20os-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['hyp_boot', 'poweroff']},
        # 300s, not 60s: every guest console byte, page-table walk and second
        # stage fault traps through the host, so a guest boot is orders of
        # magnitude slower than a host boot of the same kernel.
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        # Same argv as smoke-hyp-vcpu; -cpu rv64,h=true is load-bearing (the
        # default rv64 CPU has no H extension and hyp_supported() would refuse
        # every call, making this gate pass or fail against nothing).
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # marker_seen was non-zero: the guest reached its own banner.  The
            # marker counter covers guest console bytes only, so this cannot be
            # satisfied by the host's identical "A20OS Kernel" line.
            'HYP_A20OS: PASS',
        ],
        'forbid': ['LOCK-STALL', 'MCS DEADLOCK', 'HYP_A20OS: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-a20os: PASS; log saved to $log',
    },
    # /hypvm is the same guest boot with its parameters on the command line
    # (user/cmds/core/hypvm.c), so this gate is smoke-hyp-a20os run with argv
    # instead of with it compiled in: bare `hypvm`, i.e. exactly the defaults
    # hyp_boot carries.  Its partner is smoke-hyp-vm-96 below, which runs the
    # same program on a 96 MiB window with a bootargs string of its own.
    #
    # ONE guest boot per host boot, deliberately.  Two hypvm runs in one shell
    # session were tried first and do not work: the first run PASSes and the
    # second one (default arguments, so nothing about the arguments explains it)
    # never produces a single guest console byte and never returns --
    # hyp_vcpu_run() spins at 100% CPU with no host trap message at all.  The
    # two parameter combinations therefore live in two gates, one QEMU boot
    # each, rather than one gate with two shell lines; the limitation itself is
    # written up in docs/hypervisor/01-a20os-guest.md §10 item 9.
    'smoke-hyp-vm': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-vm-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['hypvm', 'poweroff']},
        # 300s, same as smoke-hyp-a20os: every guest console byte, page-table
        # walk and second-stage fault traps through the host, so a guest boot is
        # orders of magnitude slower than a host boot of the same kernel.
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        # Same argv as smoke-hyp-a20os; -cpu rv64,h=true is load-bearing (the
        # default rv64 CPU has no H extension, hyp_supported() refuses every
        # call, and hypvm reports that as its own exit code 4 rather than a
        # silent pass).
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # marker_seen=1 is the whole verdict (device-model counter over
            # guest console bytes only); console_bytes=[0-9]+ is the second half
            # of the same judgement hyp_boot makes, a marker length's worth of
            # output would be a degenerate hit.  exit= keeps the reason on the
            # record: the guest has no rootfs, so it is expected to end in a
            # second-stage fault rather than a shutdown.  mem= is what says this
            # really ran on the default window.
            #
            # rx_bytes= is the v3 ingress counter and is NOT asserted here: this
            # gate types nothing while the guest runs, so it has no business
            # pinning a value.  It is pinned in smoke-hyp-console, and it is
            # matched with rx_bytes=[0-9]+ rather than left out because the field
            # sits BETWEEN console_bytes= and exit=, so an older regex that
            # spelled the neighbours adjacently stops matching the moment the
            # counter exists -- which is exactly what happened when v3 landed.
            r'HYPVM: PASS marker_seen=1 console_bytes=[0-9]+ '
            r'rx_bytes=[0-9]+ exit=[0-9]+\(\w+\) mem=128 MiB',
        ],
        # Same discipline as smoke-hyp-a20os: no PANIC, because a guest panic's
        # bytes are the host kernel's own panic bytes and the guest's arrival
        # path is allowed to end there.  A host that actually dies cannot print
        # the verdict lines the expectations require.
        'forbid': ['LOCK-STALL', 'MCS DEADLOCK', 'HYPVM: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-vm: PASS; log saved to $log',
    },
    # The second hypvm gate: same program, a window that is not the default, and
    # a /chosen/bootargs of its own.  This is the run hyp_boot cannot express at
    # all -- the FDT address is derived from the window top, so a window other
    # than 128 MiB is exactly the case where a fixed DTB GPA lands inside the
    # guest image (docs/hypervisor/01-a20os-guest.md §11.3).
    #
    # No spaces inside -b: the guest shell would split them into a second argv
    # entry, and hypvm would (rightly) reject it as an unknown argument.
    'smoke-hyp-vm-96': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-vm-96-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ',
                  'lines': ['hypvm -m 96 -b a20.hypguest=1,a20.hypvm=smoke',
                            'poweroff']},
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            r'HYPVM: PASS marker_seen=1 console_bytes=[0-9]+ '
            r'rx_bytes=[0-9]+ exit=[0-9]+\(\w+\) mem=96 MiB',
            # The bootargs string went into the synthesized FDT's /chosen, which
            # is the only way to tell -b apart from a run where the flag was
            # silently dropped.
            r'HYPVM: guest=\S+ elf_bytes=\d+ mem=96 MiB base=0x80000000 '
            r"marker='A20OS Kernel' bootargs='a20.hypguest=1,a20.hypvm=smoke'",
        ],
        'forbid': ['LOCK-STALL', 'MCS DEADLOCK', 'HYPVM: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-vm-96: PASS; log saved to $log',
    },
    # The console round trip end to end, on the real guest: boot the
    # RAMFS_USER=1 kernel (which has /bin/init linked in, so it needs no block
    # device behind the stage-2), and require rx_bytes to be non-zero on the
    # trailing HYPVM line.
    #
    # rx_bytes is the device model's own ingress counter, so it is the one
    # number here that the HOST cannot fake: it increments only inside
    # hyp_dev_pump_rx(), which only runs on a guest trap, and only when
    # uart_try_getc() actually handed over a byte.  A guest that booted and
    # printed its banner without ever trapping would leave it at 0, so this
    # asserts that host keystrokes reached the ring while the guest was live.
    #
    # It deliberately does NOT claim the guest read them back.  That needs the
    # guest-side uart_rx_is_polled downgrade, which is not in this slice; the
    # round trip itself is asserted at the device boundary by
    # smoke-hyp-console-p0, where the guest program IS the RBR reader.  The
    # measured counterpart on this gate is that the real guest issues ZERO RBR
    # reads -- grep the log for 'scause=15.*stval=ffffffc010000000' and get 0
    # hits, against 11 LSR reads (…0005, scause=15) and 10 THR writes (the
    # same offset …0000 but scause=17) -- because it panics in kfree before any
    # shell exists.
    #
    # The last step is keyed on hypvm's own PASS line, not on the shell prompt,
    # for the reason spelled out at smoke-hyp-console-p0: hypvm destroys the VM
    # before printing PASS, and the pump only runs inside hyp_vcpu_run().  On
    # the prompt-keyed version the run ended `# oweroff` / `E: mksh: oweroff:
    # inaccessible or not found` and QEMU was killed by the 300s timeout.
    'smoke-hyp-console': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-console-riscv64.log',
        # The bytes have to reach the host's rx ring WHILE the guest is running,
        # not before the command: hypvm blocks inside the vcpu run loop and the
        # shell is not reading, so anything typed earlier was already consumed
        # by the shell.  Step 2 keys off "HYPVM: running", which hypvm prints
        # immediately before handing the CPU to the guest, and types one line
        # into the gap.  From there the host UART IRQ fills the host ring and
        # the very next guest trap's hyp_dev_pump_rx() moves it across.
        'stdin': {'kind': 'sendline_seq',
                  'expect': '# ',
                  'steps': [('\n# ', 'hypvm -k /bin/boot/guest-kernel-ramfs.elf'),
                            ('HYPVM: running\n', 'echo roundtrip'),
                            # A guest that WORKS no longer dies on its own: it
                            # sits at the mksh prompt and hypvm never returns,
                            # so keying this send on HYPVM: PASS waits for a
                            # line that only follows the very input this step
                            # must provide -- measured: 300s timeout, the log
                            # ends at the '# ' after roundtrip, and
                            # find(b'HYPVM: PASS') = -1.  Type `exit` into the
                            # GUEST shell first (mksh exits, init shuts the
                            # guest down, hypvm prints PASS from that); then
                            # the PASS-keyed step below types poweroff into the
                            # HOST shell, which is what the comment above the
                            # case is about.  The marker is the output line
                            # plus the prompt that follows it, not bare
                            # 'roundtrip': the shell's own echo of the typed
                            # command ('# echo roundtrip\r\n') contains that
                            # word too, and tail.find() is a literal substring
                            # match, not a regex.
                            ('roundtrip\n# ', 'exit'),
                            ('HYPVM: PASS', 'poweroff')]},
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # The RAMFS kernel is a different ELF from the one the other two
            # gates boot, and a typo in -k would silently fall back to the
            # default kernel and still satisfy everything below.  The size line
            # is what tells the two apart.
            r'HYPVM: guest=/bin/boot/guest-kernel-ramfs\.elf elf_bytes=[0-9]+ '
            r"mem=128 MiB base=0x80000000 marker='A20OS Kernel'",
            r'HYPVM: PASS marker_seen=1 console_bytes=[0-9]+ '
            r'rx_bytes=[1-9][0-9]* exit=[0-9]+\(\w+\) mem=128 MiB',
            # Clean shutdown rather than a 300s timeout kill; see the last
            # stdin step above and smoke.py's report(), which scores the log
            # text and never looks at QEMU's exit status.
            'System is going down for power-off NOW',
        ],
        # No PANIC forbid, for the same reason smoke-hyp-a20os has none: the
        # guest's panic bytes are the host kernel's own panic bytes, and this
        # gate's subject is the ingress counter, not how far the guest got.
        'forbid': ['LOCK-STALL', 'MCS DEADLOCK', 'HYPVM: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-hyp-console: PASS; log saved to $log',
    },
    # THE ACCEPTANCE GATE FOR "the guest reaches a shell you can type into".
    # smoke-hyp-console-p0 proved the device boundary (host byte -> ring -> a
    # guest PROGRAM that is itself the RBR reader -> back out), and
    # smoke-hyp-console proved the ingress counter moves on the real kernel.
    # Neither proves the thing the user asked for: a real A20OS guest, booted
    # with RAMFS_USER=1, sitting at an mksh prompt, executing a command this
    # harness typed and printing its output back.  That is what this gate is
    # for, and it is deliberately the strictest of the three.
    #
    # TWO WITNESSES, ASSERTED SEPARATELY, BECAUSE THEY ARE DIFFERENT CLAIMS.
    #
    #   rx_bytes=NN  -- "the keystrokes got into the guest's ingress ring".
    #     The counter only advances inside hyp_dev_pump_rx() (kernel/hyp/
    #     hyp_dev.c, the rx_bytes increment), which only runs from
    #     hyp_vcpu_handle_trap() and only when uart_try_getc() really handed a
    #     byte over.  It is a claim about the HOST's side of the boundary and
    #     nothing else -- last round's gate asserted it and nothing more, which
    #     is exactly the "bytes entered the ring, nobody read them" gap this
    #     gate exists to close.
    #
    #   the echoed command output -- "the guest READ the bytes and acted on
    #     them".  The host cannot produce these lines: for the whole run the
    #     host shell is parked inside hyp_vm_run() and is not reading stdin, and
    #     the host console's own echo is absent for the same reason (measured:
    #     no run of the round-trip line ever appears in the log, though
    #     rx_bytes=15 accounts for every byte of it).  A byte that gets this
    #     far has been through uart_try_getc() -> hyp_dev_pump_rx() -> the
    #     guest's LSR/RBR reads -> the guest's shell -> the guest's write path,
    #     and the only producer of the line is the guest.  The device-model RBR
    #     read cannot be pinned from the log instead: the bounded trap trace
    #     (HYP_TRAP_TRACE_HEAD, kernel/hyp/hyp_vcpu.c:1043) prints only the
    #     first 24 traps and the guest spends all of them printing its banner
    #     (measured in hyp-console-riscv64.log: traps #3..#23 are all LSR reads
    #     at stval=ffffffc010000005 / THR writes at ...0000000), long before a
    #     shell exists -- so a shell-time RBR read never reaches a log line at
    #     all, and pretending otherwise would make this gate unsatisfiable.
    #
    # WHY THE TOKENS LOOK LIKE THEY DO.  The output witness is matched as
    # "(?:^|# )TOKEN\r?$" rather than "^TOKEN$" because the two possible guest
    # tty echo behaviours put the token in different places: with echo the
    # token is on its own output line (the command itself is on the previous
    # line, after the prompt); with no echo it is glued to the prompt as
    # "# TOKEN".  The alternation accepts both and still refuses the host's
    # echo, which would read "# echo TOKEN\r" -- there the token is preceded by
    # "echo ", not by a line start or by "# ", so it cannot match.  The
    # trailing \r? is there because host console lines are CRLF (measured:
    # "# hypvm -k ...^M" in the same log) and smoke.py's grep_matches() is a
    # MULTILINE re.search (tools/smoke.py:203-212), where `$` sits before the
    # "\n" but not before a "\r".
    #
    # TWO COMMANDS, NOT ONE.  The second one is what turns "the shell ran a
    # command" into "the shell is interactive and loops": the guest has to
    # print its prompt again and read a second line before the second output
    # can appear, and the prompt is asserted directly by the
    # "...TOKEN\n# " expectation below.
    #
    # HOW THE RUN ENDS, AND WHY IT IS `exit` AND NOT `poweroff`.  A guest
    # session has to be closed from the guest side or hyp_vm_run() never
    # returns and the rx_bytes counter is never printed.  `exit` does it with
    # nothing added to the tree: mksh exits, user/init.c:264-283 reaps the
    # shell child, falls out of its wait loop and calls do_shutdown(), which
    # prints "[init] shutting down" and reboot(RB_POWER_OFF)s (user/init.c:
    # 137-143); the guest kernel turns that into the SBI SRST shutdown the host
    # serves as HYP_EXIT_SHUTDOWN (=1, kernel/include/hyp/hyp_vcpu.h:55), so
    # hypvm prints its PASS line with the counter.  `poweroff` cannot be typed
    # INTO the guest: the RAMFS_USER rootfs is exactly RAMFS_USER_PROGRAMS
    # (Makefile:1238-1241) and there is no poweroff binary in it, and the guest
    # has no block device behind the stage-2 to find one on.  The LAST step is
    # therefore the HOST's poweroff, keyed on hypvm's PASS line -- the pump only
    # runs inside hyp_vcpu_run(), so once the guest is gone the keystroke goes
    # back to the host shell instead of being stolen into a ring that no longer
    # exists (same reasoning as smoke-hyp-console-p0).
    'smoke-hyp-shell': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/hyp-shell-riscv64.log',
        # Every marker gates its own line, in order, on a tail that is trimmed
        # after each match (tools/run_with_timeout.py:127-133), so a marker
        # further down can only be satisfied by output produced after the ones
        # before it.  Step 3's marker is the bare first token, which the guest's
        # own echo can also produce -- harmless: the bytes queue in the ring in
        # order and the guest executes them in order, so `exit` cannot overtake
        # the output it is waiting for.
        'stdin': {'kind': 'sendline_seq', 'expect': '# ',
                  'steps': [('\n# ', 'hypvm -k /bin/boot/guest-kernel-ramfs.elf'),
                            ('HYPVM: running\n', 'echo AAAABBBBCCCC'),
                            ('AAAABBBBCCCC', 'echo DDEEEEEEFFFF'),
                            ('DDEEEEEEFFFF', 'exit'),
                            ('HYPVM: PASS', 'poweroff')]},
        # 300s, same as the other hypvm gates: every guest console byte, every
        # page-table walk and every second-stage fault traps through the host,
        # so a guest boot is orders of magnitude slower than a host boot of the
        # same kernel.
        'timeout': '300s',
        'qemu': 'qemu-system-riscv64',
        # Same argv as smoke-hyp-console; -cpu rv64,h=true is carried so a host
        # whose default rv64 has no H extension cannot make this gate pass
        # against nothing.
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-cpu', 'rv64,h=true', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': [
            # Right guest image: a typo in -k would silently fall back to the
            # default kernel and still satisfy a banner-only reading of this
            # gate.  The size line is what tells the two apart.
            r'HYPVM: guest=/bin/boot/guest-kernel-ramfs\.elf elf_bytes=[0-9]+ '
            r"mem=128 MiB base=0x80000000 marker='A20OS Kernel'",
            # WITNESS 1 -- the guest read the keystrokes and ran the command.
            r'(?:^|# )AAAABBBBCCCC\r?$',
            # The guest is back at its mksh prompt after that output, which is
            # the only way the second command below can be typed at all.  This
            # is the "reached a shell prompt" requirement, pinned on guest-side
            # text: the host prompt is on an earlier line and the tail has been
            # trimmed past the host's own steps by the time this can match.
            r'AAAABBBBCCCC\r?\n# ',
            # WITNESS 1 again, for the second command: proof the loop is
            # interactive, not one-shot.
            r'(?:^|# )DDEEEEEEFFFF\r?$',
            # The guest's init took its power-off path, i.e. the guest was a
            # live init+shell system that shut down on request rather than a
            # kernel that died.  The host's init never prints this (measured:
            # the host boot reaches "[telnetd] listening on port 2323" and sits
            # at the prompt).
            r'\[init\] shutting down',
            # WITNESS 2 -- the device model's own ingress counter, pinned
            # non-zero on the line hypvm prints after the guest is gone.  Kept
            # as a separate expectation from the two above on purpose: they
            # assert the READ, this one asserts the PUMP, and collapsing them
            # is what made the previous round's claim hollow.
            r'HYPVM: PASS marker_seen=1 console_bytes=[0-9]+ '
            r'rx_bytes=[1-9][0-9]* exit=1\(shutdown\) mem=128 MiB',
            # Clean host shutdown rather than a 300s timeout kill; smoke.py's
            # report() scores the log text and never looks at QEMU's exit
            # status, so without this a hang stays indistinguishable from a
            # pass.
            'System is going down for power-off NOW',
        ],
        # PANIC is forbidden HERE and deliberately not in smoke-hyp-console's
        # list.  There, a guest with no rootfs is an expected arrival and its
        # panic bytes are byte-identical to the host's, so a PANIC forbid would
        # go red on a correct run.  Here the guest must reach a shell, so a
        # panic of either side is the failure this gate exists to catch, and
        # both sides print the same strings -- which is exactly why both
        # patterns are here.
        'forbid': ['KERNEL PANIC', r'\[PANIC\]', 'SLAB BUG', 'LOCK-STALL',
                   'MCS DEADLOCK', 'HYPVM: FAIL',
                   # "the guest came up and the keystrokes never reached it".
                   # The expect above already requires rx_bytes non-zero; this
                   # is here so the failure message names the cause instead of
                   # printing a missing-pattern list.
                   r'rx_bytes=0\b'],
        'timeout_msg': True,
        'pass_msg': 'smoke-hyp-shell: PASS; log saved to $log',
    },
    'smoke-mmprobe': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mmprobe-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['mmprobe', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['MMPROBE: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mmprobe: PASS; log saved to $log',
    },
    # SMP-only reproducer for the multi-threaded corruption report.  Every
    # earlier gate that shares an mm_struct ran on a single CPU, where the
    # fault-around window can never lose its VMA to a sibling thread, so the
    # defect was invisible to the whole matrix.  cpus=4 plus CONFIG_SLAB_DEBUG=1
    # is the combination that fails without the fix: the slab check is what
    # turns a stray write into a panic here instead of a wrong answer later.
    'smoke-mtcorrupt': {
        'gate': {'mem': '1G', 'cpus': '4'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0', 'NR_CPUS=4', 'CONFIG_SLAB_DEBUG=1'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mtcorrupt-riscv64-smp4.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['mtcorrupt_test', 'poweroff']},
        'timeout': '180s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '4', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-slabdbg/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-slabdbg/ext4.img,if=none,format=raw,id=x1', '-device', 'virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-slabdbg/isofs.img,if=none,format=raw,id=x2', '-device', 'virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp4-slabdbg/kernel.elf'],
        'expect': ['MTCORRUPT: PASS'],
        'forbid': ['SIGSEGV', 'SLAB DEBUG', 'FATAL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-mtcorrupt: PASS; log saved to $log',
    },
    'smoke-mntns': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/mntns-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['mntns_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['MNTNS_TEST: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-mntns: PASS; log saved to $log',
    },
    'smoke-pidns': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/pidns-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['pidns_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['PIDNS_TEST: PASS'],
        'forbid': ['SIGSEGV', 'PIDNS_TEST: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-pidns: PASS; log saved to $log',
    },
    'smoke-userns': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/userns-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['userns_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['USERNS_TEST: PASS'],
        'forbid': ['SIGSEGV', 'USERNS_TEST: FAIL'],
        'timeout_msg': False,
        'pass_msg': 'smoke-userns: PASS; log saved to $log',
    },
    'smoke-native-contract': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-contract-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-contract-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['ralg ok', 'bp ok', 'evqc ok', 'vmol ok', 'dma ok', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-contract: PASS; log saved to $log',
    },
    'smoke-native-cluster': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-cluster-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-cluster-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['abi-nr ok', 'abi-node ok', 'abi-stub ok', 'abi-args ok', 'abi-ver ok', 'NATIVE_CLUSTER: PASS', 'System is going down for power-off NOW'],
        'forbid': ['SIGSEGV'],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-cluster: PASS; log saved to $log',
    },
    'smoke-native-debug': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-debug-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-debug-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_DEBUG: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-debug: PASS; log saved to $log',
    },
    'smoke-native-deepen': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-deepen-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-deepen-rv', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_DEEPEN: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-deepen: PASS; log saved to $log',
    },
    'smoke-native-dynlink': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-dynlink-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/dynprobe-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['FAKELD:.*ok', 'DYNPROBE: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-dynlink: PASS; log saved to $log',
    },
    'smoke-native-ext': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-ext-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-ext-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_EXT: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-ext: PASS; log saved to $log',
    },
    'smoke-native-fs-all': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-fs-all-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/svcmgr-rv &', 'sleep 2', '/bin/ufs_all_test', 'poweroff']},
        'timeout': '180s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ufs-scratch.img,if=none,format=raw,id=xfs1', '-device', 'virtio-blk-device,drive=xfs1,bus=virtio-mmio-bus.2', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ufs-ext4.img,if=none,format=raw,id=xfs2', '-device', 'virtio-blk-device,drive=xfs2,bus=virtio-mmio-bus.4', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ufs-iso.img,if=none,format=raw,id=xfs3', '-device', 'virtio-blk-device,drive=xfs3,bus=virtio-mmio-bus.6', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ufs-ntfs.img,if=none,format=raw,id=xfs4', '-device', 'virtio-blk-device,drive=xfs4,bus=virtio-mmio-bus.7', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.ufsd_blk=1'],
        'expect': ['UXFS_ALL: PASS', 'System is going down for power-off NOW', 'SVC_MGR: ufsd blk=1 (cmdline)'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-fs-all: PASS; log saved to $log',
    },
    'smoke-native-futex': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-futex-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-futex-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_FUTEX: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-futex: PASS; log saved to $log',
    },
    'smoke-native-handle': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-handle-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-handle-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['part ok', 'tchan ok', 'bch ok', 'evq ok', 'opc ok', 'ac ok', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-handle: PASS; log saved to $log',
    },
    'smoke-native-ipc': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-ipc-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-ipc-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_IPC: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-ipc: PASS; log saved to $log',
    },
    'smoke-native-isolation': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-isolation-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-isolation-rv', 'poweroff']},
        'timeout': '90s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_ISOLATION: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-isolation: PASS; log saved to $log',
    },
    'smoke-native-libc': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-libc-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-libc-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_LIBC: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-libc: PASS; log saved to $log',
    },
    'smoke-native-linux': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-linux-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-linux-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['linux fd ok', 'linux mmap ok', 'linux pipe ok', 'linux sockpair ok', 'linux futex ok', 'linux epoll ok', 'NATIVE_LINUX: PASS', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-linux: PASS; log saved to $log',
    },
    'smoke-native-mm': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-mm-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-mm-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_MM: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-mm: PASS; log saved to $log',
    },
    'smoke-native-personality': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-personality-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-personality-rv', '/bin/pipe_ref', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_PERSONALITY: PASS', 'System is going down for power-off'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-personality: PASS (native + Linux ABI reference agree); log saved to $log',
    },
    'smoke-native-registry': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-registry-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/svcmgr-rv &', 'sleep 1', '/bin/native-registry-rv', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_REGISTRY: PASS', 'SVC_MGR: ready', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-registry: PASS; log saved to $log',
    },
    'smoke-native-rtcd': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-rtcd-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-rtcd-rv', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_RTCD: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-rtcd: PASS; log saved to $log',
    },
    'smoke-native-shmring': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-shmring-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-shmring-rv', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_SHMRING: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-shmring: PASS; log saved to $log',
    },
    'smoke-native-signal': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-signal-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/native-signal-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_SIGNAL: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-signal: PASS; log saved to $log',
    },
    'smoke-native-svc': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-svc-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/svcman-rv', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['NATIVE_SVC: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-svc: PASS; log saved to $log',
    },
    'smoke-native-ubd': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-ubd-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/ubd_fs_test', 'poweroff']},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ubd-scratch.img,if=none,format=raw,id=xubd', '-device', 'virtio-blk-device,drive=xubd,bus=virtio-mmio-bus.3', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['UBD_FS: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-ubd: PASS; log saved to $log',
    },
    'smoke-native-ufs': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/native-ufs-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/ufs_test', 'poweroff']},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/ufs-scratch.img,if=none,format=raw,id=xufs', '-device', 'virtio-blk-device,drive=xufs,bus=virtio-mmio-bus.2', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['UXFS_FS: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-native-ufs: PASS; log saved to $log',
    },
    'smoke-netctl': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/netctl-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['netctl', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
        'expect': ['NETCTL: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-netctl: PASS; log saved to $log',
    },
    'smoke-network-suite': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/network-suite-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['network_suite', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
        'expect': ['NETWORK_SUITE: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-network-suite: PASS; log saved to $log',
    },
    'smoke-network-suite-aarch64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=aarch64', 'BOARD=qemu-virt-aarch64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/network-suite-aarch64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['network_suite', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-aarch64',
        'argv': ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a57', '-m', '1G', '-nographic', '-smp', '1', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/aarch64-qemu-virt-aarch64-linux-dev/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
        'expect': ['NETWORK_SUITE: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-network-suite-aarch64: PASS; log saved to $log',
    },
    'smoke-oom-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/oom-stress-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['oom_stress', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['OOM_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-oom-stress: PASS; log saved to $log',
    },
    'smoke-pci-portability': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['rm -f user/build/loongarch64/hda.a20drv user/build/loongarch64/nvme.a20drv',
                'mkdir -p .kernel-build && rm -f .kernel-build/nvme-scratch.img && truncate -s 64M .kernel-build/nvme-scratch.img'],
        'build': {'vars': ['ARCH=loongarch64', 'BOARD=qemu-virt-loongarch64', 'ABI=both', 'BRINGUP=0', 'CONFIG_HDA_SMOKE_TEST=y', 'DRVMOD_SMOKE=1'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/pci-portability-loongarch64.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-loongarch64',
        'argv': ['qemu-system-loongarch64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-snapshot', '-audiodev', 'driver=none,id=audio0', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio0', '-drive', 'file=.kernel-build/nvme-scratch.img,if=none,format=raw,id=nvme0', '-device', 'nvme,drive=nvme0,serial=A20NVME', '-drive', 'file=.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev-hda-smoke/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev-hda-smoke/kernel.elf'],
        'expect': ['HDA_STREAM_SMOKE: PASS', 'NVME_CAP_SMOKE: PASS', 'NVME_IO_SMOKE: PASS', '\\[NVME\\] driver registered in core: 0'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-pci-portability: PASS; log saved to $log',
    },
    'smoke-poll-edge': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/poll-edge-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['poll_edge', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['POLL_EDGE: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-poll-edge: PASS; log saved to $log',
    },
    'smoke-proc-a20': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/proc-a20-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['cat /proc/a20/bcache', 'cat /proc/a20/page_cache', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['^valid_pages:', '^capacity:'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-proc-a20: PASS; log saved to $log',
    },
    'smoke-proc-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/proc-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['proc_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['PROC_STRESS: PASS', 'PROC_STRESS: signal-stop-exit PASS', 'PROC_STRESS: signal-mask-park PASS', 'PROC_STRESS: thread-exec-cloexec PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-proc-stress: PASS; log saved to $log',
    },
    'smoke-procfs-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/procfs-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['procfs_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['PROCFS_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-procfs-stress: PASS; log saved to $log',
    },
    'smoke-ptrace': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/ptrace-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['ptrace_smoke', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['PTRACE_SMOKE: PASS'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-ptrace: PASS; log saved to $log',
    },
    # The CMOS RTC half of the x86_64 wall clock.  -rtc base=utc is pinned so
    # the guest clock and the host clock are comparable without the driver
    # having to apply a local-time offset (it does not: see the module).
    # The in-guest markers prove the wall clock came from the RTC rather than
    # from the build timestamp; the host-side check proves the reported clock
    # is real time.  Neither alone is enough, so both are asserted.
    'smoke-rtc-cmos': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/rtc-cmos-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/date -u +RTC_WALLCLOCK=%Y-%m-%dT%H:%M:%SZ', 'poweroff']},
        'timeout': '30s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-rtc', 'base=utc', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'post': ['python3 tools/check_rtc_wallclock.py --max-skew 300 .kernel-build/smoke/rtc-cmos-x86_64.log'],
        'expect': ['\\[CMOS-RTC\\] driver registered in core: 0',
                   '\\[CMOS-RTC\\] wall clock: \\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}',
                   '\\[TIME\\] wallclock: hardware RTC adopted, unix=1[0-9]{9}',
                   '\\[TIME\\] wallclock: no RTC readable yet, seed from build time',
                   'RTC_WALLCLOCK=\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z',
                   'System is going down for power-off'],
        'forbid': ['PANIC|Kernel panic|cmos-rtc.*unresolved symbol'],
        'timeout_msg': True,
        'pass_msg': 'smoke-rtc-cmos: PASS (cmos-rtc.a20drv bound, wall clock from CMOS, guest clock matches the host); log saved to $log',
    },
    # The same boot with a CMOS the driver must refuse: QEMU's RTC is set to
    # 1960, outside the window the driver accepts, so the read fails, the
    # driver binds anyway and logs why, and the kernel keeps the build-time
    # seed.  This is the fallback path: it must not panic, and it must not
    # claim it adopted a hardware clock.
    'smoke-rtc-cmos-fallback': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=both', 'BRINGUP=0', 'DRIVER_DEPLOYMENT=generic'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/rtc-cmos-fallback-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['poweroff']},
        'timeout': '30s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-rtc', 'base=1960-06-15T12:00:00', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': ['\\[CMOS-RTC\\] no usable time',
                   '\\[TIME\\] wallclock: no RTC readable yet, seed from build time',
                   'System is going down for power-off'],
        'forbid': ['\\[CMOS-RTC\\] wall clock:', 'hardware RTC adopted', 'PANIC|Kernel panic'],
        'timeout_msg': True,
        'pass_msg': 'smoke-rtc-cmos-fallback: PASS (out-of-window CMOS refused, wall clock kept the build-time seed, no panic); log saved to $log',
    },
    'smoke-pty-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/pty-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['pty_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['pty_stress: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-pty-stress: PASS; log saved to $log',
    },
    'smoke-sched-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/sched-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['sched_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SCHED_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-sched-stress: PASS; log saved to $log',
    },
    'smoke-scm-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/scm-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['scm_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SCM_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-scm-stress: PASS; log saved to $log',
    },
    'smoke-signalfd-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/signalfd-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['signalfd_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SIGNALFD_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-signalfd-stress: PASS; log saved to $log',
    },
    'smoke-smp-bringup': {
        'gate': {'mem': '1G', 'cpus': '2'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=1', 'NR_CPUS=2', 'ALLOW_UNVERIFIED_SMP=1'], 'target': 'kernel-only'},
        'log': '.kernel-build/smoke/riscv64-smp2-bringup.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '2', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-bringup-smp2/kernel.elf'],
        'expect': ['part ok', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-smp-bringup: PASS; log saved to $log',
    },
    'smoke-socket-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/socket-stress-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['socket_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SOCKET_STRESS: PASS'],
        'forbid': ['\\[LOCK\\]'],
        'timeout_msg': True,
        'pass_msg': 'smoke-socket-stress: PASS; log saved to $log',
    },
    'smoke-swap': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/swap-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['swap_test', 'poweroff']},
        'timeout': '45s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SWAP_TEST: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-swap: PASS; log saved to $log',
    },
    'smoke-syscall-ext': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/syscall-ext-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['syscall_ext', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['SYSCALL_EXT: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-syscall-ext: PASS; log saved to $log',
    },
    'smoke-timeout-test': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/timeout-test-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['timeout_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['TIMEOUT_TEST: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-timeout-test: PASS; log saved to $log',
    },
    'smoke-timer-edge': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/timer-edge-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['timer_edge', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['TIMER_EDGE: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-timer-edge: PASS; log saved to $log',
    },
    # xHCI interrupt path.  Beyond "the keyboard and the mouse enumerate",
    # this asserts the three things that only hold when completions are driven
    # by the controller's own INTx line rather than by the core's global poll:
    #   1. probe claimed a line (`completion=interrupt` on the ready line),
    #   2. the handler actually ran and consumed ring entries (`irq=` > 0),
    #   3. a key pressed through QMP arrives as an EV_KEY (code 30 == 'a').
    #
    # QEMU's `usb-kbd` reports only when its state changes, so an idle guest
    # completes nothing and assertions 2 and 3 would be untestable without an
    # injection.  The injection goes through QMP rather than the muxed stdio
    # monitor because `sendkey` routes to the first console-less input handler
    # -- see qmp_key_pump() in tools/smoke.py for why that is the PS/2 keyboard
    # here and not the USB one.
    'smoke-usb-x86_64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/usb-x86_64.log',
        'stdin': None,
        'timeout': '30s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd', '-device', 'usb-mouse', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        # Pressed across the whole window rather than once: the guest has no
        # interrupt endpoint for the keyboard until it has enumerated it, so a
        # single early press would prove nothing.
        'qmp': {'keys': ['a'], 'hold': 0.05, 'gap': 0.3, 'duration': 18.0},
        'expect': ['\\[USB-HID\\] keyboard ready', '\\[USB-HID\\] mouse ready',
                   '\\[XHCI\\] controller ready: .*completion=interrupt',
                   '\\[XHCI\\] completions: irq=[1-9]',
                   '\\[USB-HID\\] key event: code=30 value=1'],
        'forbid': ['\\[USB\\] port.*enumeration failed', '\\[XHCI\\].*failed',
                   '\\[XHCI\\] controller ready: .*completion=polling'],
        'timeout_msg': False,
        'pass_msg': 'smoke-usb-x86_64: PASS; log saved to $log',
    },
    # A USB hub on the xHCI root bus.  The hub is a genuine class-9 device,
    # so this proves the class driver binds, reads the hub descriptor with
    # the class request code it really uses (0xA0, not the standard
    # GET_DESCRIPTOR that every hub stalls), derives the port bitmap size
    # from bNbrPorts, arms the status-change interrupt endpoint through the
    # parent controller and publishes a second bus for the core to scan.
    #
    # Scope: QEMU's `usb-hub` has no downstream bus (QEMU 9 dropped it, and
    # `-device usb-kbd,bus=hub0.0` now fails), so nothing can be hung behind
    # it, and its port bitmap reports the last two phantom ports as
    # connected — an off-by-two that the reset then fails to clear.  So the
    # forbid list below pins down what must NOT fail: no root port of the
    # xHCI controller, and no hub-internal step.  The two phantom downstream
    # ports are expected to fail here and say nothing about the driver.
    'smoke-usb-hub-x86_64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/usb-hub-x86_64.log',
        'stdin': None,
        'timeout': '25s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-device', 'qemu-xhci,id=xhci', '-device', 'usb-hub,id=hub0,bus=xhci.0', '-device', 'usb-kbd', '-device', 'usb-mouse', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': [
            '\\[USB\\] device 0409:55aa port=\\d+ speed=\\d+',
            "\\[USB-HUB\\] hub 0409:55aa: downstream ports=10 status_bytes=3 ss=0",
            '\\[USB-HUB\\] status-change endpoint 81 armed: mps=\\d+ interval=\\d+',
            '\\[USB-HUB\\] downstream bus live: 10 ports behind 0409:55aa',
            '\\[USB-HID\\] keyboard ready',
            '\\[USB-HID\\] mouse ready',
        ],
        'forbid': [
            '\\[USB\\] port [1-8] enumeration failed',
            '\\[XHCI\\].*failed',
            '\\[USB-HUB\\].*failed',
            '\\[USB-HUB\\] no interrupt IN endpoint',
            '\\[USB-HUB\\] malformed hub descriptor',
        ],
        'timeout_msg': False,
        'pass_msg': 'smoke-usb-hub-x86_64: PASS; log saved to $log',
    },
    # virtio-scsi command completion on the used-ring interrupt, end to end.
    #
    # The boot disk stays virtio-blk-pci on purpose: it is the path already
    # known to work, so a failure here localises to the scsi controller rather
    # than to "the machine came up at all".  The scsi-hd behind
    # virtio-scsi-pci is the scratch medium the guest test writes.
    #
    # Two independent claims, because they fail for different reasons:
    #   * the loopback assertions (write / flush / read back, three rounds with
    #     an in-place rewrite) say the data plane survived moving completion
    #     off the poll -- a completion path that skipped the response DMA sync
    #     would still return the wrong bytes;
    #   * `irq_count` in the guest's own stats line says the interrupt path was
    #     the one that delivered them.  It is bumped only by the top-half, so a
    #     driver that had silently fallen back to polling passes the loopback
    #     and fails this.  `completion=poll` in the forbid list is the same
    #     claim from the kernel side.
    #
    # The guest picks the device by probing /dev/diskN for the stats ioctl
    # rather than by index, so the case does not depend on which class slot
    # the controller was given.
    'smoke-virtio-scsi-irq': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['rm -f /tmp/a20-virtio-scsi-irq.img',
                'dd if=/dev/zero of=/tmp/a20-virtio-scsi-irq.img bs=1M count=64 2>/dev/null'],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/virtio-scsi-irq-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 20, 'lines': ['virtio_scsi_test', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot',
                 '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0',
                 '-device', 'virtio-blk-pci,drive=x0',
                 '-drive', 'file=/tmp/a20-virtio-scsi-irq.img,if=none,format=raw,id=xs',
                 '-device', 'virtio-scsi-pci,id=scsi0',
                 '-device', 'scsi-hd,drive=xs,bus=scsi0.0,scsi-id=0,lun=0',
                 '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': [
            r'\[VIRTIO-SCSI\] disk ready: \d+ sectors \(\d+ MiB\), completion=(msix|intx) irq=-?\d+',
            r'VIRTIO_SCSI_TEST: /dev/disk\d+ capacity=\d+ bytes irq_mode=[12] irq_line=-?\d+ irq_count=\d+',
            r'VIRTIO_SCSI_TEST: stats commands=\d+ flushes=[1-9]\d* irq_count=\d+ \(\+[1-9]\d*\) irq_completions=\d+ spin_completions=\d+ timeouts=0',
            'VIRTIO_SCSI_TEST: PASS',
        ],
        'forbid': [
            'VIRTIO_SCSI_TEST: FAIL',
            r'\[VIRTIO-SCSI\] .*completion polling',
            r'\[VIRTIO-SCSI\] .*registration failed',
            r'\[VIRTIO-SCSI\] request timeout',
            r'\[VIRTIO-SCSI\] SCSI command \w+ failed',
            r'\[VIRTIO-PCI\] .*incomplete capabilities',
        ],
        'post': ['rm -f /tmp/a20-virtio-scsi-irq.img'],
        'timeout_msg': False,
        'pass_msg': 'smoke-virtio-scsi-irq: PASS; log saved to $log',
    },
    # Message-signalled interrupts, end to end, on the one machine type where
    # the kernel programs a real interrupt controller: x86_64's LAPIC.  Every
    # other board has no message-signalled path at all, so this case is where
    # the code that is arch-independent (capability parsing, table location,
    # vector reservation, teardown) and the code that is x86-only (LVT
    # programming, the message address) meet for the first time.
    #
    # Two devices with two different table layouts are on the bus on purpose.
    # QEMU's virtio-pci puts its MSI-X table in BAR1 (msix_init_exclusive_bar,
    # msix_bar_idx = 1) and the e1000e's in BAR3; both publish the location in
    # the capability's Message Address Lower field using the pre-PCIe encoding,
    # because -kernel boots without firmware to write Vector Control.  A driver
    # that guessed a BAR, or that read only Vector Control, programs the wrong
    # window -- the readback check in pci_msix_program_vector() catches that,
    # so the forbid list below is what says it did not happen.
    #
    # The delivery line is the point of the whole case: it is printed from the
    # interrupt handler the first time a message-signalled completion arrives,
    # so it cannot appear unless a device really posted a message, the platform
    # really took it, and the kernel really dispatched it to the handler that
    # was registered on that vector.  Getting the message address wrong -- ORing
    # the vector into the LAPIC page, say -- produces a correctly programmed
    # table that never interrupts anything, and this line is what fails.
    #
    # The machine now runs two CPUs and the case drives every programmed vector
    # off the boot processor part-way through.  -smp 1 was enough while the
    # destination was hard-wired to APIC ID 0, but the second half of this case
    # is exactly that it no longer is: a per-CPU destination is a different
    # message address AND an LVT entry inside a different processor's own LAPIC
    # page, and neither is exercised by a uniprocessor boot.  Everything
    # asserted before the move is unchanged -- the delivery line is the same
    # one, on the same vector, now carrying the CPU that took it.
    #
    # `/proc/a20/irq_affinity` is written once the shell is up: the move is a
    # runtime property (mask, rewrite the address, arm the far LVT over IPI,
    # unmask), and folding it into probe would make the boot-CPU delivery this
    # case already proves untestable.  The `cat` first is the falsifiable
    # half -- if the node did not exist the write would be a shell error, and
    # the second delivery line would never appear either way.
    'smoke-msix-x86_64': {
        'gate': {'mem': '1G', 'cpus': '2'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0', 'NR_CPUS=2'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/msix-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 20,
                  'lines': ['cat /proc/a20/irq_affinity',
                            'echo 1 > /proc/a20/irq_affinity',
                            'ls /bin',
                            'cat /proc/a20/irq_affinity',
                            'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '2', '-no-reboot', '-net', 'nic,model=e1000e', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev-smp2/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev-smp2/kernel.elf'],
        'expect': [
            r'\[MSI-X\] pci-1af4:1001-\d+: capability at 0x[0-9a-f]+, table \d+ entries in BAR1\+0x0 \(Message Address Low, pba BAR\d+\), 1 requested',
            r'\[VIRTIO-PCI\] pci-1af4:1001-\d+: MSI-X reserved, vectors 208\.\.208 for 1 queue\(s\)',
            r'\[MSI-X\] pci-1af4:1001-\d+: enabled, 1 vector\(s\) armed',
            r'\[VIRTIO-BLK\] pci-1af4:1001-\d+ using MSI-X vectors 208\.\.208 completions',
            r'\[VIRTIO-BLK\] MSI-X delivery on vector 208',
            r'\[MSI-X\] pci-8086:10d3-\d+: capability at 0x[0-9a-f]+, table \d+ entries in BAR3\+0x0 \(Message Address Low, pba BAR\d+\), 2 requested',
            r'\[E1000\] MSI-X enabled on vectors 209\.\.210',
            # The move itself, then delivery on the CPU it was moved to.
            r'\[MSI-X\] affinity: \d+ vector\(s\) now target cpu 1',
            r'\[VIRTIO-BLK\] MSI-X delivery on vector 208 cpu=1',
            # The readback after the move: virtio-blk's entry now names cpu 1
            # instead of the 0 the first cat printed.  The pre-move cat makes
            # the same line end in \t0, so this pattern only the later one
            # satisfies -- which is what stops "the node exists" from passing
            # as "the node says what it just did".
            r'pci-1af4:1001-\d+\t0\t208\t1',
        ],
        'forbid': [
            r'\[MSI-X\] .*capability names no table',
            r'\[MSI-X\] .*platform has no message-signalled interrupt path',
            r'\[MSI-X\] .*this window is not an MSI-X table',
            r'\[VIRTIO-BLK\] .*MSI-X handler registration failed',
            r'\[VIRTIO-PCI\] .*MSI-X unavailable',
            r'\[MSI-X\] .*controller entry could not be armed',
            r'\[MSI-X\] .*entry \d+ refused cpu 1',
            r'\[X86_64 MSI-X\] .*timed out',
        ],
        'timeout_msg': False,
        'pass_msg': 'smoke-msix-x86_64: PASS; log saved to $log',
    },
    'smoke-vfs-edge': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/vfs-edge-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['vfs_edge', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
        'expect': ['VFS_EDGE: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-vfs-edge: PASS; log saved to $log',
    },
    'smoke-vfs-stress': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/vfs-stress-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['vfs_stress', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/ext4.img,if=none,format=raw,id=x1', '-device', 'virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/isofs.img,if=none,format=raw,id=x2', '-device', 'virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['VFS_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-vfs-stress: PASS; log saved to $log',
    },
    'smoke-lfs': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['make -s ARCH=riscv64 ABI=linux BRINGUP=0 .kernel-build/riscv64-qemu-virt-riscv64-both-dev/lfs.img', 'test -x user/build/riscv64/lfs_test'],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/lfs-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['lfs_test', 'poweroff']},
        'timeout': '45s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/lfs.img,if=none,format=raw,id=xlfs', '-device', 'virtio-blk-device,drive=xlfs,bus=virtio-mmio-bus.1', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['LITTLEFS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-lfs: PASS; log saved to $log',
    },
    'smoke-pivot-root': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['test -x user/build/riscv64/pivot_root_test'],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/pivot-root-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['pivot_root_test', 'poweroff']},
        'timeout': '45s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf'],
        'expect': ['PIVOT_ROOT: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-pivot-root: PASS; log saved to $log',
    },
    'smoke-vfs-stress-smp2': {
        'gate': {'mem': '1G', 'cpus': '2'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0', 'NR_CPUS=2'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/vfs-stress-smp2-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['vfs_stress', 'poweroff']},
        'timeout': '60s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '2', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp2/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp2/ext4.img,if=none,format=raw,id=x1', '-device', 'virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp2/isofs.img,if=none,format=raw,id=x2', '-device', 'virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp2/kernel.elf'],
        'expect': ['VFS_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-vfs-stress-smp2: PASS; log saved to $log',
    },
    'smoke-vfs-stress-smp8': {
        'gate': {'mem': '1G', 'cpus': '8'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0', 'NR_CPUS=8'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/vfs-stress-smp8-riscv64.log',
        'stdin': {'kind': 'sendline', 'expect': '# ', 'lines': ['vfs_stress', 'poweroff']},
        'timeout': '90s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '8', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/ext4.img,if=none,format=raw,id=x1', '-device', 'virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/isofs.img,if=none,format=raw,id=x2', '-device', 'virtio-blk-device,drive=x2,bus=virtio-mmio-bus.2', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev-smp8/kernel.elf'],
        'expect': ['VFS_STRESS: PASS'],
        'forbid': [],
        'timeout_msg': False,
        'pass_msg': 'smoke-vfs-stress-smp8: PASS; log saved to $log',
    },
    'smoke-virtio-sound': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': ['rm -f /tmp/a20-smoke-audio.wav'],
        'build': {'vars': ['ARCH=x86_64', 'BOARD=qemu-virt-x86_64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/virtio-sound-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/audioplay --tone 440 --duration 5000', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-snapshot', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-audiodev', 'driver=wav,id=audio0,path=/tmp/a20-smoke-audio.wav', '-device', 'virtio-sound-pci,audiodev=audio0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-linux-dev/kernel.elf'],
        'expect': ['audioplay: 440 Hz for 5000 ms -> /dev/audio', 'audioplay: playback complete', 'System is going down for power-off NOW'],
        'forbid': ['audioplay: playback failed'],
        'post': ['python3 tools/check_wav_pcm.py --min-frames 8000 /tmp/a20-smoke-audio.wav'],
        'timeout_msg': False,
        'pass_msg': 'smoke-virtio-sound: PASS; log=$log wav=/tmp/a20-smoke-audio.wav',
    },
    'smoke-unix-ch': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/unix-ch-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/unix_ch_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['UNIX_CH: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-unix-ch: PASS; log saved to $log',
    },
    'smoke-bpf': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/bpf-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/bpf_smoke', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf'],
        'expect': ['BPF_SMOKE: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-bpf: PASS; log saved to $log',
    },
    'smoke-wx-aslr': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/wx-aslr-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['/bin/wx_aslr_test', 'poweroff']},
        'timeout': '30s',
        'qemu': 'qemu-system-riscv64',
        # a20.wx=deny explicitly, because this case tests the *deny policy*,
        # not the default.  The default became `off` (stock-Linux semantics)
        # once nodejs showed that a JIT writes into the pages it asks to be
        # executable -- see kernel/mm/wx.c.  Hardening stays available and
        # still needs a test, so the test opts into it rather than assuming
        # it is what a boot gives you.
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.wx=deny'],
        'expect': ['WX_ASLR: PASS', 'System is going down for power-off NOW'],
        'forbid': [],
        'timeout_msg': True,
        'pass_msg': 'smoke-wx-aslr: PASS; log saved to $log',
    },
    # Foreign-architecture translation.  xlate_exec execve()s the x86_64
    # probe without naming a translator, so this only passes if the kernel
    # forwarded it.
    #
    # Four different things have to hold, and each is a separate failure mode
    # that a single "it translated" assertion would hide:
    #
    #   ok            the channel works at all;
    #   enoexec       a file that is not a runnable ELF is still ENOEXEC,
    #                 i.e. the hook does not go looking past the header
    #                 checks for anything it might translate;
    #   unconfigured  a *valid* ELF naming e_machine=183 -- a registered
    #                 guest -- is still ENOEXEC, because no translator was
    #                 configured for it.  This is the line that separates
    #                 "the administrator provisioned a translator" from "the
    #                 kernel was built knowing about aarch64";
    #   script        a #! script still reaches its interpreter;
    #   toggle        /proc/a20/xlator really is a switch: the same binary
    #                 translates, stops translating after `write 0`, and
    #                 translates again after `write 1`, with no reboot.
    #
    # The cmdline sets a20.xlator but NOT a20.wx=off -- the translator's JIT
    # buffer is allowed by the per-task exemption, so a blanket W^X
    # relaxation must not be needed and the policy must still read "deny".
    'smoke-exec-xlator': {
        'gate': {'mem': '1G', 'cpus': '1'},
        # XLATOR=1, not a `pre` step: the probe and the translator land in
        # $(USER_BUILD_DIR), which `make -C user clean` wipes whenever the
        # userspace build id changes.  As an image prerequisite make orders
        # them after that clean; as a pre step they were silently deleted one
        # invocation later and the smoke failed on a missing binary.
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0',
                           'XLATOR=1'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/exec-xlator-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 40, 'lines': [
            '/bin/xlate_exec ok /bin/xlate_probe-x86_64 SMOKE',
            '/bin/xlate_exec enoexec ignored',
            '/bin/xlate_exec unconfigured ignored',
            # x86_64 *is* configured on this boot, so an ENOEXEC here can
            # only come from the ABI half of the key missing: a
            # native-ABI image of a guest that has a Linux-ABI translator
            # must not be handed to it.
            '/bin/xlate_exec native ignored',
            '/bin/xlate_exec script ignored',
            '/bin/xlate_exec toggle /bin/xlate_probe-x86_64 TOGGLE',
            'poweroff',
        ]},
        'timeout': '150s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.wx=deny a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'],
        'expect': [
            r'\[XLATOR] x86_64 \(e_machine=62 abi=linux\) → /bin/qemu-x86_64',
            # The registry has two guests and only one was provisioned, so
            # boot has to name the other rather than leaving the admin to
            # wonder why their aarch64 binary is a bare ENOEXEC.
            r'\[XLATOR] aarch64 \(e_machine=183 abi=linux\) 未配置翻译器',
            r'\[XLATOR\] pid=\d+ execve /bin/xlate_probe-x86_64 \(e_machine=62 abi=linux\)',
            r'\[WX\] \S+: pid=\d+ 翻译器宿主，放行 W\|X',
            'XLATE_PROBE: MARK=SMOKE ARGC=2',
            'XLATE_EXEC: ok PASS: exit=42',
            'XLATE_EXEC: enoexec PASS: ENOEXEC',
            'XLATE_EXEC: unconfigured PASS: ENOEXEC',
            'XLATE_EXEC: native PASS: ENOEXEC',
            'XLATE_EXEC: script PASS: exit=0',
            # The runtime switch loop over one binary.  Ordering matters
            # here in a way it does not for the other lines: step2's ENOEXEC
            # is only meaningful because step1 proved the same binary ran.
            r'XLATE_EXEC: toggle: node present, enabled=1',
            r'XLATE_EXEC: toggle: step1 execve while enabled PASS \(exit=42\)',
            r'XLATE_EXEC: toggle: step2 wrote 0, node reads enabled=0',
            r'XLATE_EXEC: toggle: step2 execve while disabled PASS \(ENOEXEC\)',
            r'XLATE_EXEC: toggle: step3 wrote 1, node reads enabled=1',
            r'XLATE_EXEC: toggle: step3 execve after re-enable PASS \(exit=42\)',
            r'XLATE_EXEC: toggle: step4 junk writes rejected EINVAL, still enabled=1',
            'XLATE_EXEC: toggle PASS',
            'System is going down for power-off NOW',
        ],
        # W^X has to be *deny* for the translator's JIT buffer to prove
        # anything.  Under the tree's default (off -- kernel/mm/wx.c keeps it
        # that way on purpose) the per-task exemption would fire and assert
        # nothing, because nothing was ever refused; and the forbid below
        # names the policy line precisely so the case cannot silently run
        # with the policy it is not about.  So the append sets a20.wx=deny
        # explicitly, and this then tests the real claim: the translator is
        # allowed by an exemption on the task, not by turning W^X off.  No
        # mode may report a failure or an unusable wait.
        'forbid': [r'W\^X 策略: off', r'XLATE_EXEC: \w+ FAIL', r'waitpid\(\d+\)'],
        'timeout_msg': True,
        'pass_msg': 'smoke-exec-xlator: PASS; log saved to $log',
    },
    # The same channel on loongarch64, where the interesting part is not the
    # translation but the *configuration surface*: QEMU's loongarch virt hands
    # the guest no kernel command line by any route (four routes tried and all
    # negative -- see docs/exec-xlator/01-usage.md), so until now `a20.*` keys
    # were compiled in and unreachable on this architecture: CONFIG_XLATOR=y,
    # /proc/a20/xlator registered, and no way to ever turn it on.
    #
    # The command line therefore has to be typed on the serial port, which
    # costs this case two things the riscv64 one does not need:
    #
    #   UART_CMDLINE=y  compiles the console reader in.  Off by default: it
    #     moves authority over kernel configuration from whoever built the
    #     image to whoever holds the serial cable at boot.
    #   -serial stdio -monitor none -display none instead of -nographic,
    #     because -nographic is -serial mon:stdio and QEMU's mux never hands
    #     the bytes to the guest's 16550 (measured: the kernel prints the
    #     prompt and times out having seen nothing).
    #
    # And it cannot be a 'pipe' stdin: QEMU feeds the host's pipe to the
    # emulated UART as soon as there are bytes, which is well before the guest
    # programs the 16550, so whatever arrived first is overwritten in the
    # one-byte holding register.  Hence sendline_seq -- each line waits for its
    # own marker, and the first marker is the kernel asking.
    #
    # The `# ` shell marker is anchored with a preceding newline because the
    # boot banner contains `# ` inside its ASCII art; `\n# ` matches only the
    # real prompt.  Markers are matched in order, so the later ones cannot fire
    # on output that belongs to an earlier step.
    'smoke-exec-xlator-la64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'build': {'vars': ['ARCH=loongarch64', 'ABI=both', 'BRINGUP=0',
                           'XLATOR=1', 'UART_CMDLINE=y'],
                  'target': 'dev-build'},
        'log': '.kernel-build/smoke/exec-xlator-la64.log',
        'stdin': {'kind': 'sendline_seq', 'steps': [
            ('[UARTCMD] ',
             'a20.wx=deny a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'),
            ('\n# ', '/bin/xlate_exec ok /bin/xlate_probe-x86_64 SMOKE'),
            ('XLATE_EXEC: ok PASS', 'poweroff'),
        ]},
        'timeout': '300s',
        'qemu': 'qemu-system-loongarch64',
        'argv': ['qemu-system-loongarch64', '-machine', 'virt', '-m', '1G',
                 '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
                 '-smp', '1', '-drive',
                 'file=.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/fat32.img,if=none,format=raw,id=x0',
                 '-device', 'virtio-blk-pci,drive=x0',
                 '-netdev', 'user,id=net', '-device', 'virtio-net-pci,netdev=net',
                 '-kernel',
                 '.kernel-build/loongarch64-qemu-virt-loongarch64-both-dev/kernel.elf'],
        'expect': [
            # Two separate claims.  The first is that the serial path ran at
            # all; the second is that its result reached bootargs_get() and
            # not just the console -- without the second, a kernel that printed
            # a convincing prompt and threw the string away would pass.
            r'\[UARTCMD\] using command line from the console',
            r"\[FDT\] bootargs='a20\.wx=deny a20\.xlator=1 "
            r"a20\.xlator\.x86_64=/bin/qemu-x86_64'",
            r'\[XLATOR] x86_64 \(e_machine=62 abi=linux\) → /bin/qemu-x86_64',
            r'\[XLATOR] aarch64 \(e_machine=183 abi=linux\) 未配置翻译器',
            r'\[XLATOR\] pid=\d+ execve /bin/xlate_probe-x86_64 \(e_machine=62 abi=linux\)',
            # Same W^X claim as on riscv64, and for the same reason it has
            # to be asked for explicitly: the tree's default is off, and the
            # translator's JIT buffer is allowed by the per-task exemption
            # rather than by weakening the policy.
            r'\[WX\] \S+: pid=\d+ 翻译器宿主，放行 W\|X',
            'XLATE_PROBE: MARK=SMOKE ARGC=2',
            'XLATE_EXEC: ok PASS: exit=42',
            'System is going down for power-off NOW',
        ],
        'forbid': [r'W\^X 策略: off', r'XLATE_EXEC: \w+ FAIL',
                   r'no command line given',
                   r'waitpid\(\d+\)'],
        'timeout_msg': True,
        'pass_msg': 'smoke-exec-xlator-la64: PASS; log saved to $log',
    },
    # The other end of CONFIG_XLATOR.  smoke-exec-xlator shows the channel
    # works; this shows it can be absent, which is the property an embedded
    # or low-resource build is actually buying.  It is asserted three ways,
    # because "the code is gone" and "the behaviour is gone" are different
    # claims and only the second one matters to a user:
    #
    #   * the foreign probe is present in the image and its execve is
    #     ENOEXEC (checked with `xlate_exec foreign`, which verifies the file
    #     exists before asserting -- a missing file would also be ENOEXEC and
    #     would prove nothing);
    #   * no [XLATOR] line appears anywhere in the boot log -- not even a
    #     "disabled" notice, because in a cut-down build the configuration
    #     code is not compiled at all, so there is nothing to notice;
    #   * /proc/a20/xlator does not exist, so there is no switch to write.
    #
    # The cmdline still says a20.xlator=1 and still names a translator path.
    # That is the point: with the knob compiled out the cmdline must have no
    # effect whatsoever, rather than being honoured by a stub.
    'smoke-exec-xlator-off': {
        'gate': {'mem': '1G', 'cpus': '1'},
        # XLATOR=1 here even though the kernel has the channel compiled out:
        # the image still has to *contain* the foreign probe for
        # `xlate_exec enoexec /bin/xlate_probe-x86_64` to have something to
        # be rejected.
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0',
                           'CONFIG_XLATOR=0', 'XLATOR=1'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/exec-xlator-off-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 40, 'lines': [
            # `foreign`, not `enoexec`: enoexec fabricates its own fixture and
            # would report ENOEXEC for a file that was never in the image.
            # `foreign` execs the real cross-built probe and first checks it
            # exists, so ENOEXEC here can only mean "present, and refused".
            '/bin/xlate_exec foreign /bin/xlate_probe-x86_64',
            'cat /proc/a20/xlator',
            'poweroff',
        ]},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev-noxlator/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev-noxlator/kernel.elf', '-append', 'a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'],
        'expect': [
            'XLATE_EXEC: foreign PASS: ENOEXEC',
            # Positive proof the switch node is gone, rather than "the string
            # does not appear anywhere" -- which the shell's own echo of the
            # command would satisfy while proving nothing.
            r'open /proc/a20/xlator: No such file or directory',
            'System is going down for power-off NOW',
        ],
        # The remaining absence assertion is the kernel's own: a [XLATOR] line
        # here would mean the configuration code is still compiled in.  There
        # is no "disabled" notice either, because in a cut-down build there is
        # no code left to print one.
        'forbid': [r'\[XLATOR\]', r'XLATE_EXEC: \w+ FAIL'],
        'timeout_msg': True,
        'pass_msg': 'smoke-exec-xlator-off: PASS; log saved to $log',
    },

    # Plugging in a differently-shaped translator.  smoke-exec-xlator proves
    # the channel works with qemu-user; this proves the *invocation* is
    # configuration rather than code, which is what lets a translator that
    # wants the Rosetta-style shape (`<path> <args...>`, no argv[0] option)
    # or any other shape be pointed at without a kernel change.
    #
    # It needs no download and no cross compiler: the target is
    # user/cmds/core/xlate_shim.c, which translates nothing and just prints
    # the argv and environment it was handed, and the guest is a header this
    # image's own xlate_exec synthesises.  Asserting against a program that
    # does nothing but print is the point -- a real translator *tolerates* a
    # wrong argv, so a smoke using one cannot tell "the kernel built what the
    # template said" from "the translator coped".
    #
    # Both registered guests are configured at once and pointed at the same
    # shim, so this also covers the table dispatching on e_machine with more
    # than one live entry -- the case the single-guest smokes leave untested.
    #
    #   x86_64   default template overridden to "@P,@*"  -> [shim, path, args]
    #            with ROSETTA-ish env injected; no stray argv[0] argument
    #   aarch64  registry default "-0 @A @P @*"         -> [shim, -0, decoy,
    #            path, args] with the caller's argv[0] preserved
    #
    # The argv[0] in the aarch64 case is a decoy passed as `--argv0=`, because
    # execv() would otherwise make argv[0] and the path the same string and a
    # broken @A substitution would be invisible.
    'smoke-exec-xlator-shim': {
        'gate': {'mem': '1G', 'cpus': '1'},
        # No XLATOR=1: nothing has to be downloaded or cross-built, which is
        # what keeps this smoke hermetic and fast.
        'build': {'vars': ['ARCH=riscv64', 'ABI=both', 'BRINGUP=0'],
                  'target': 'dev-build'},
        'log': '.kernel-build/smoke/exec-xlator-shim-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 40, 'lines': [
            '/bin/xlate_exec stage 62 /tmp/guest_x86_64',
            '/bin/xlate_exec stage 183 /tmp/guest_aarch64',
            '/bin/xlate_exec run /tmp/guest_x86_64 ALPHA BETA',
            '/bin/xlate_exec run --argv0=decoy-argv0 /tmp/guest_aarch64 ALPHA',
            # The sharp form of the ABI check, and the reason it is here
            # rather than only in smoke-exec-xlator: both machines are
            # configured at this point, so a channel keyed on the machine
            # alone would hand this file to the shim -- and the shim prints
            # its argv, which is what the two new forbids below look for.
            '/bin/xlate_exec native ignored',
            'poweroff',
        ]},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.xlator=1 a20.xlator.x86_64=/bin/xlate_shim a20.xlator.x86_64.argv=@P,@* a20.xlator.x86_64.env=XLATOR_TEST_ENV=hello a20.xlator.aarch64=/bin/xlate_shim'],
        'expect': [
            # Both guests configured at once, each with the template that will
            # actually be used -- the override called out as such.
            r'\[XLATOR\] x86_64 \(e_machine=62 abi=linux\) → /bin/xlate_shim  argv="@P @\*" \(cmdline 覆盖\)',
            r'\[XLATOR\] aarch64 \(e_machine=183 abi=linux\) → /bin/xlate_shim  argv="-0 @A @P @\*"',
            r'\[XLATOR\] pid=\d+ execve /tmp/guest_x86_64 \(e_machine=62 abi=linux\) → /bin/xlate_shim  argv="@P @\*"',
            # Overridden template: path first, no argv[0] option, no stray
            # positional in front of the path.  argv[1] being the path (not
            # the decoy, and not a leftover argv[0]) is the assertion that
            # the old per-guest flag column could not express.
            r'XLATE_SHIM: argv\[0\]=/bin/xlate_shim',
            r'XLATE_SHIM: argv\[1\]=/tmp/guest_x86_64',
            r'XLATE_SHIM: argv\[2\]=ALPHA',
            r'XLATE_SHIM: argv\[3\]=BETA',
            # Injected environment, from a20.xlator.x86_64.env.
            r'XLATE_SHIM: env XLATOR_TEST_ENV=hello',
            # Registry default: the -0 option and the caller's argv[0], which
            # is a decoy rather than the path -- so this line can only be
            # produced by a correct @A substitution.
            r'XLATE_SHIM: argv\[1\]=-0',
            r'XLATE_SHIM: argv\[2\]=decoy-argv0',
            r'XLATE_SHIM: argv\[3\]=/tmp/guest_aarch64',
            r'XLATE_SHIM: argv\[4\]=ALPHA',
            'XLATE_SHIM: done',
            # Refused at the lookup, not after it: neither the shim's argv
            # nor the kernel's own forward line may ever name this file.  A
            # positive ENOEXEC alone would not distinguish "the key missed"
            # from "the translator declined it", and only the first keeps the
            # outcome a diagnostic instead of a fault inside a program that
            # cannot load the file.
            'XLATE_EXEC: native PASS: ENOEXEC',
            'System is going down for power-off NOW',
        ],
        'forbid': [r'XLATE_EXEC: \w+ FAIL', r'XLATE_SHIM: argv\[\d+\]=$',
                   r'XLATE_SHIM: \S*xlate_native',
                   r'\[XLATOR\] pid=\d+ execve /tmp/xlate_native\.elf'],
        'timeout_msg': True,
        'pass_msg': 'smoke-exec-xlator-shim: PASS; log saved to $log',
    },
}
