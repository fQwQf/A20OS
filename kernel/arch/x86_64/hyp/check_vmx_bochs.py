#!/usr/bin/env python3
"""Execute the real VMX/EPT backend with Bochs 3.0 (VMX=2 + debugger).
Bochs executes VMX instructions in software, including VMCS validity checks.
No host VMX capability, root, GRUB, rootfs or GUI is required.
"""
import argparse
from pathlib import Path
import subprocess
import time

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bochs-dir',type=Path,required=True,help='built Bochs source directory containing bochs and bios/')
    ap.add_argument('--kernel',type=Path,required=True,help='fresh kernel.bin built with BRINGUP=1')
    ap.add_argument('--cpus',type=int,default=1)
    ap.add_argument('--without-virtualization',action='store_true')
    ap.add_argument('--timeout',type=float,default=120)
    ap.add_argument('--output',type=Path,default=Path('.kernel-build/vmx-bochs'))
    args=ap.parse_args();b=args.bochs_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    template=Path(__file__).with_name('bochs_loader.S.in')
    bootargs='a20.hyp_test=1'
    if args.cpus>1:bootargs+=' a20.hyp_smp=1'
    source=out/'loader.S';source.write_text(template.read_text().replace('@BOOTARGS@',bootargs))
    subprocess.run(['as','--32',str(source),'-o',str(out/'loader.o')],check=True)
    subprocess.run(['ld','-m','elf_i386','-Ttext','0x7c00','--oformat','binary',str(out/'loader.o'),'-o',str(out/'floppy.img')],check=True)
    with (out/'floppy.img').open('r+b') as f:f.truncate(1474560)
    chunks=out/'chunks';chunks.mkdir(exist_ok=True)
    cmds=['pb 0x7c00','c'];data=args.kernel.read_bytes()
    # Bochs 3.0 loadmem doesn't advance its address between 4096-byte reads.
    # Supply separate chunks to exercise the unmodified upstream emulator.
    for off in range(0,len(data),4096):
        chunk=chunks/f'{off:x}.bin';chunk.write_bytes(data[off:off+4096]);cmds.append(f'loadmem "{chunk}" {0x200000+off:#x}')
    cmds.append('c');(out/'debug.rc').write_text('\n'.join(cmds)+'\n')
    excluded=', exclude_features="vmx"' if args.without_virtualization else ''
    (out/'bochsrc').write_text(f'''megs: 512
romimage: file={b}/bios/BIOS-bochs-latest
vgaromimage: file={b}/bios/VGABIOS-lgpl-latest.bin
floppya: 1_44={out}/floppy.img, status=inserted
boot: floppy
cpu: model=corei7_sandy_bridge_2600k, count={args.cpus}, add_features="1g_pages"{excluded}, ips=50000000
clock: sync=none
com1: enabled=1, mode=file, dev={out}/serial.log
display_library: nogui
log: {out}/bochs.log
panic: action=fatal
error: action=report
info: action=report
''')
    serial=out/'serial.log';serial.unlink(missing_ok=True)
    with (out/'console.log').open('w') as console:
        proc=subprocess.Popen([str(b/'bochs'),'-dbg','-f',str(out/'bochsrc'),'-rc',str(out/'debug.rc')],stdin=subprocess.PIPE,stdout=console,stderr=subprocess.STDOUT,text=True)
        until=time.monotonic()+args.timeout
        while proc.poll() is None and time.monotonic()<until:
            text=serial.read_text(errors='replace') if serial.exists() else ''
            if 'System is going down for power-off NOW' in text:break
            time.sleep(.1)
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
    text=serial.read_text(errors='replace') if serial.exists() else ''
    required=['HYP_SELFTEST: PASS','HYP_ARCH_TEST: NX+RO PASS','HYP_ARCH_TEST: PASS backend=x86-vmx rounds=3','HYP_ARCH_TEST: preemption+stop PASS','part ok','System is going down for power-off NOW']
    if args.cpus>1:required.append('HYP_SMP_TEST: PASS')
    if args.without_virtualization:required=['HYP_ARCH_TEST: SKIP (backend unavailable)','part ok','System is going down for power-off NOW']
    missing=[m for m in required if m not in text]
    forbidden=['HYP_ARCH_TEST: FAIL','HYP_SMP_TEST: FAIL','HYP_SELFTEST: FAIL','PANIC','Kernel Panic']
    found=[marker for marker in forbidden if marker in text]
    if missing or found:
        print(f'FAIL missing={missing} forbidden={found}; logs: {out}\n{text[-3000:]}');return 1
    label='VMX unavailable negative control' if args.without_virtualization else f'real VMX/EPT execution cpus={args.cpus}'
    print(f'PASS: {label}; logs: {out}');return 0
if __name__=='__main__':raise SystemExit(main())
