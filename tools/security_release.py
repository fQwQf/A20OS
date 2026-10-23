#!/usr/bin/env python3
"""Run the SECURITY candidate or the two arms of its bounded cost comparison."""
import argparse
import copy
from pathlib import Path
import smoke


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['candidate', 'perf-off', 'perf-on', 'boot'])
    args = parser.parse_args()
    case = copy.deepcopy(smoke.CASES['smoke-security-release'])
    case['name'] = 'security-' + args.mode
    case['log'] = '.kernel-build/smoke/security-' + args.mode + '-riscv64.log'
    if args.mode.startswith('perf-'):
        enabled = args.mode == 'perf-on'
        case['stdin']['lines'] = ['exec security_release_test --bench-' + ('on' if enabled else 'off')]
        case['expect'] = ['SECURITY_PERF: PASS', 'sample=1', 'sample=2', 'sample=3',
                          r'\[SD\] POWER_OFF entry', 'audit errors=0']
        case['forbid'] = ['SECURITY_PERF: FAIL', 'PANIC|Kernel panic|Kernel BUG|LOCK-STALL']
        if enabled:
            case['expect'] += ['UEDUD: dev0 faulted', 'UEDUD: dev1 unaffected',
                               'UEDUD: domains released']
        else:
            argv = case['argv']
            without_iommu = []
            i = 0
            while i < len(argv):
                if argv[i] == '-device' and argv[i + 1].startswith(('riscv-iommu-pci,', 'edu,')):
                    i += 2
                else:
                    without_iommu.append(argv[i])
                    i += 1
            case['argv'] = without_iommu
            case['argv'][-1] = 'a20.wx=off'
    elif args.mode == 'boot':
        # This is the ordinary shell control. The candidate uses exec to let
        # independent probes finish even when W^X breaks shell SIGCHLD handling.
        case['stdin']['lines'] = ['/bin/echo SECURITY_BOOT_OK', 'poweroff']
        case['argv'][-1] = 'a20.wx=off'
        case['expect'] = ['SECURITY_BOOT_OK', 'System is going down for power-off',
                          'audit errors=0']
        case['forbid'] = ['PANIC|Kernel panic|Kernel BUG|LOCK-STALL']
    case['pass_msg'] = case['name'] + ': PASS'
    smoke.run_gate(case)
    smoke.run_build(case)
    case = smoke.resolve_build_paths(case, smoke.resolve_build_dir(case))
    status = smoke.run_qemu(case)
    print(f"{case['name']}: QEMU exit={status}")
    result = smoke.report(case['name'], case, status)
    for line in Path(case['log']).read_text(errors='replace').splitlines():
        if any(marker in line for marker in ('SECURITY_RELEASE:', 'SECURITY_PERF:',
                                             'WX_ASLR:', 'SANDBOX:', 'CRED_STATE_TEST:',
                                             'ENVELOPE_SMOKE:', 'UEDUD:')):
            print(line)
    return result


if __name__ == '__main__':
    raise SystemExit(main())
