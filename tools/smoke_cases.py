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
    'smoke-io-event': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=riscv64', 'ABI=linux', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/io-event-riscv64.log',
        'stdin': {'kind': 'pipe', 'delay': 8, 'lines': ['io_event_test', 'poweroff']},
        'timeout': '20s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel.elf', '-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os'],
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
    'smoke-usb-x86_64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/usb-x86_64.log',
        'stdin': None,
        'timeout': '20s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd', '-device', 'usb-mouse', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': ['\\[USB-HID\\] keyboard ready', '\\[USB-HID\\] mouse ready'],
        'forbid': ['\\[USB\\] port.*enumeration failed', '\\[XHCI\\].*failed'],
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
    'smoke-msix-x86_64': {
        'gate': {'mem': '1G', 'cpus': '1'},
        'pre': [],
        'build': {'vars': ['ARCH=x86_64', 'ABI=both', 'BRINGUP=0'], 'target': 'dev-build'},
        'log': '.kernel-build/smoke/msix-x86_64.log',
        'stdin': {'kind': 'pipe', 'delay': 18, 'lines': ['poweroff']},
        'timeout': '45s',
        'qemu': 'qemu-system-x86_64',
        'argv': ['qemu-system-x86_64', '-machine', 'q35', '-m', '1G', '-nographic', '-smp', '1', '-no-reboot', '-net', 'nic,model=e1000e', '-drive', 'file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-pci,drive=x0', '-kernel', '.kernel-build/x86_64-qemu-virt-x86_64-both-dev/kernel.elf'],
        'expect': [
            r'\[MSI-X\] pci-1af4:1001-\d+: capability at 0x[0-9a-f]+, table \d+ entries in BAR1\+0x0 \(Message Address Low, pba BAR\d+\), 1 requested',
            r'\[VIRTIO-PCI\] pci-1af4:1001-\d+: MSI-X reserved, vectors 208\.\.208 for 1 queue\(s\)',
            r'\[MSI-X\] pci-1af4:1001-\d+: enabled, 1 vector\(s\) armed',
            r'\[VIRTIO-BLK\] pci-1af4:1001-\d+ using MSI-X vectors 208\.\.208 completions',
            r'\[VIRTIO-BLK\] MSI-X delivery on vector 208',
            r'\[MSI-X\] pci-8086:10d3-\d+: capability at 0x[0-9a-f]+, table \d+ entries in BAR3\+0x0 \(Message Address Low, pba BAR\d+\), 2 requested',
            r'\[E1000\] MSI-X enabled on vectors 209\.\.210',
        ],
        'forbid': [
            r'\[MSI-X\] .*capability names no table',
            r'\[MSI-X\] .*platform has no message-signalled interrupt path',
            r'\[MSI-X\] .*this window is not an MSI-X table',
            r'\[VIRTIO-BLK\] .*MSI-X handler registration failed',
            r'\[VIRTIO-PCI\] .*MSI-X unavailable',
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
            '/bin/xlate_exec script ignored',
            '/bin/xlate_exec toggle /bin/xlate_probe-x86_64 TOGGLE',
            'poweroff',
        ]},
        'timeout': '150s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'],
        'expect': [
            r'\[XLATOR\] x86_64 \(e_machine=62\) → /bin/qemu-x86_64',
            # The registry has two guests and only one was provisioned, so
            # boot has to name the other rather than leaving the admin to
            # wonder why their aarch64 binary is a bare ENOEXEC.
            r'\[XLATOR\] aarch64 \(e_machine=183\) 未配置翻译器',
            r'\[XLATOR\] pid=\d+ execve /bin/xlate_probe-x86_64 \(e_machine=62\)',
            r'\[WX\] \S+: pid=\d+ 翻译器宿主，放行 W\|X',
            'XLATE_PROBE: MARK=SMOKE ARGC=2',
            'XLATE_EXEC: ok PASS: exit=42',
            'XLATE_EXEC: enoexec PASS: ENOEXEC',
            'XLATE_EXEC: unconfigured PASS: ENOEXEC',
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
        # W^X must stay at its default deny (the translator is allowed by
        # the per-task exemption, not by disabling the policy), and no mode
        # may report a failure or an unusable wait.
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
             'a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'),
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
            r"\[FDT\] bootargs='a20\.xlator=1 a20\.xlator\.x86_64=/bin/qemu-x86_64'",
            r'\[XLATOR\] x86_64 \(e_machine=62\) → /bin/qemu-x86_64',
            r'\[XLATOR\] aarch64 \(e_machine=183\) 未配置翻译器',
            r'\[XLATOR\] pid=\d+ execve /bin/xlate_probe-x86_64 \(e_machine=62\)',
            # Same W^X claim as on riscv64: the translator's JIT buffer is
            # allowed by the per-task exemption, so the policy itself stays
            # at deny.
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
            'poweroff',
        ]},
        'timeout': '120s',
        'qemu': 'qemu-system-riscv64',
        'argv': ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-nographic', '-smp', '1', '-bios', 'default', '-global', 'virtio-mmio.force-legacy=false', '-drive', 'file=.kernel-build/riscv64-qemu-virt-riscv64-both-dev/fat32.img,if=none,format=raw,id=x0', '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0', '-netdev', 'user,id=net', '-device', 'virtio-net-device,netdev=net,bus=virtio-mmio-bus.4', '-kernel', '.kernel-build/riscv64-qemu-virt-riscv64-both-dev/kernel.elf', '-append', 'a20.xlator=1 a20.xlator.x86_64=/bin/xlate_shim a20.xlator.x86_64.argv=@P,@* a20.xlator.x86_64.env=XLATOR_TEST_ENV=hello a20.xlator.aarch64=/bin/xlate_shim'],
        'expect': [
            # Both guests configured at once, each with the template that will
            # actually be used -- the override called out as such.
            r'\[XLATOR\] x86_64 \(e_machine=62\) → /bin/xlate_shim  argv="@P @\*" \(cmdline 覆盖\)',
            r'\[XLATOR\] aarch64 \(e_machine=183\) → /bin/xlate_shim  argv="-0 @A @P @\*"',
            r'\[XLATOR\] pid=\d+ execve /tmp/guest_x86_64 \(e_machine=62\) → /bin/xlate_shim  argv="@P @\*"',
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
            'System is going down for power-off NOW',
        ],
        'forbid': [r'XLATE_EXEC: \w+ FAIL', r'XLATE_SHIM: argv\[\d+\]=$'],
        'timeout_msg': True,
        'pass_msg': 'smoke-exec-xlator-shim: PASS; log saved to $log',
    },
}
