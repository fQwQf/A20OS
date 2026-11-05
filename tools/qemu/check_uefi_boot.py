#!/usr/bin/env python3
"""Boot the standalone x86 UEFI loader under OVMF and require clean S5 exit."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
from a20_resource import Policy, Requirement, gate


def validate_boot(output: str, status: int) -> None:
    markers = ('[RAM] usable', '[ACPI] uACPI', 'namespace ready',
               'part ok', 'System is going down for power-off NOW')
    missing = [marker for marker in markers if marker not in output]
    if status or missing or 'KERNEL PANIC' in output:
        raise ValueError(f'UEFI boot failed: exit={status}, missing={missing}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--efi', type=Path, required=True)
    parser.add_argument('--code', type=Path, default=Path(os.environ.get('OVMF_CODE', '/usr/share/OVMF/OVMF_CODE_4M.fd')))
    parser.add_argument('--vars', type=Path, default=Path(os.environ.get('OVMF_VARS', '/usr/share/OVMF/OVMF_VARS_4M.fd')))
    parser.add_argument('--log', type=Path, default=ROOT / '.kernel-build/smoke/uefi-x86_64.log')
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    for path in (args.efi, args.code, args.vars):
        if not path.is_file():
            parser.error(f'required firmware/artifact missing: {path}')
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        parser.error('qemu-system-x86_64 is required')
    policy = Policy.from_env()
    gate(Requirement(512 + policy.reserve_mem_mb, 1, policy.min_disk_mb),
         policy, ROOT, wait=False, label='UEFI x86 smoke')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='a20-uefi-') as temporary:
        stage = Path(temporary)
        esp = stage / 'esp'
        boot = esp / 'EFI/BOOT'
        boot.mkdir(parents=True)
        shutil.copyfile(args.efi, boot / 'BOOTX64.EFI')
        variables = stage / 'vars.fd'
        shutil.copyfile(args.vars, variables)
        command = [qemu, '-machine', 'q35', '-cpu', 'max,pdpe1gb=off',
                   '-m', '512M', '-smp', '1', '-display', 'none',
                   '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-net', 'none',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={args.code.resolve()}',
                   '-drive', f'if=pflash,format=raw,file={variables}',
                   '-drive', f'if=none,id=esp,format=raw,file=fat:rw:{esp}',
                   '-device', 'virtio-blk-pci,drive=esp']
        status = -1
        with args.log.open('wb') as log:
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.timeout, check=False)
                status = result.returncode
            except subprocess.TimeoutExpired:
                pass
        try:
            validate_boot(args.log.read_text(errors='replace'), status)
        except ValueError as error:
            print(f'{error}; log: {args.log}')
            return 1
    print(f'smoke-uefi-x86_64: PASS; log: {args.log}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
