#!/usr/bin/env python3
"""Exercise DT -> reserved RAM -> existing simplefb/fbdev -> actual QEMU pixels.

The host firmware fixture configures QEMU ramfb before releasing the stopped
CPU. A20OS receives only a standard DT handoff; it never reads fw_cfg. This is
an emulated firmware scanout, not evidence that a board's bootloader enables
its HDMI/LCD controller.
"""
import argparse
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from a20_resource import Policy, Requirement, gate
from raspberrypi4_fixture import Tree
from check_uefi_framebuffer import verify_bars
ROOT=Path(__file__).resolve().parents[1]

def inject(blob, config):
    _,_,structure,strings,reserves,*_=struct.unpack_from('>10I',blob)
    strings=blob[strings:]
    tree=Tree();offset=structure;path=[];chosen=False
    while True:
        token=struct.unpack_from('>I',blob,offset)[0];offset+=4
        if token==1:
            end=blob.index(0,offset);name=blob[offset:end].decode();offset=(end+4)&~3
            path.append(name);tree.begin(name)
            if path==['','chosen']:chosen=True
        elif token==2:
            if path==['','chosen']:add_framebuffer(tree,config)
            if path==[''] and not chosen:
                tree.begin('chosen');add_framebuffer(tree,config);tree.end()
            tree.end();path.pop()
        elif token==3:
            size,name=struct.unpack_from('>II',blob,offset);offset+=8
            end=strings.index(0,name);name=strings[name:end].decode()
            value=blob[offset:offset+size];offset=(offset+size+3)&~3
            if not(path==['','chosen'] and name=='bootargs'):tree.prop(name,value)
        elif token==4:continue
        elif token==9:break
        else:raise ValueError('malformed QEMU-generated DTB')
    held=[]
    while True:
        address,size=struct.unpack_from('>QQ',blob,reserves);reserves+=16
        if not size:break
        held.append((address,size))
    return tree.finish(held)

def add_framebuffer(tree,cfg):
    tree.text('bootargs','a20.hostname=dt-framebuffer-test')
    tree.begin('framebuffer@%x'%cfg['framebuffer'])
    tree.text('compatible','simple-framebuffer')
    size=(cfg['stride']*cfg['height']+4095)&~4095
    tree.cells('reg',0,cfg['framebuffer'],0,size)
    for key in ('width','height','stride'):tree.cells(key,cfg[key])
    tree.text('format','x8r8g8b8');tree.end()

class Debugger:
    def __init__(self,path):
        self.socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.socket.settimeout(5);self.socket.connect(str(path))
    def command(self,body):
        encoded=body.encode()
        self.socket.sendall(b'$'+encoded+b'#'+f'{sum(encoded)&255:02x}'.encode())
        ack=self.socket.recv(1)
        if ack!=b'+':raise ValueError(f'GDB packet rejected: {ack!r}')
        if body=='c':return ''
        while self.socket.recv(1)!=b'$':pass
        result=bytearray()
        while True:
            byte=self.socket.recv(1)
            if byte==b'#':break
            if not byte:raise ValueError('GDB disconnected')
            result+=byte
        checksum=self.socket.recv(2)
        if int(checksum,16)!=(sum(result)&255):raise ValueError('GDB checksum')
        self.socket.sendall(b'+')
        return result.decode()
    def write(self,address,data):
        if self.command(f'M{address:x},{len(data):x}:'+data.hex())!='OK':
            raise ValueError('firmware fixture write refused')
    def read(self,address,size):
        return bytes.fromhex(self.command(f'm{address:x},{size:x}'))
    def scanout(self,cfg):
        base=cfg['fw_cfg'];self.write(base+8,struct.pack('>H',0x19))
        def data(size):return bytes(self.read(base,1)[0] for _ in range(size))
        count=struct.unpack('>I',data(4))[0]
        if count>256:raise ValueError('invalid fw_cfg directory')
        selected=None
        for _ in range(count):
            entry=data(64)
            if entry[8:].split(b'\0',1)[0]==b'etc/ramfb':
                size,selected=struct.unpack_from('>IH',entry)
                if size!=28:raise ValueError('unexpected ramfb protocol size')
        if selected is None:raise ValueError('ramfb firmware endpoint absent')
        transfer=cfg['dma'];payload=transfer+64
        self.write(payload,struct.pack('>QIIIII',cfg['framebuffer'],0x34325258,0,
                                     cfg['width'],cfg['height'],cfg['stride']))
        self.write(transfer,struct.pack('>IIQ',(selected<<16)|0x18,28,payload))
        self.write(base+16,struct.pack('>Q',transfer))
        if self.read(transfer,4)!=bytes(4):raise ValueError('ramfb firmware DMA failed')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--platform',type=Path,default=ROOT/'kernel/arch/aarch64/boot/tests/ramfb.json')
    parser.add_argument('--output',type=Path,default=ROOT/'.kernel-build/smoke/dt-framebuffer')
    parser.add_argument('--timeout',type=float,default=120)
    args=parser.parse_args()
    cfg=json.loads(args.platform.read_text())
    qemu=shutil.which(cfg['qemu'])
    if not qemu or not args.kernel.is_file():parser.error('kernel artifact and QEMU required')
    policy=Policy.from_env();gate(Requirement(cfg['memory_mb']+policy.reserve_mem_mb,1,policy.min_disk_mb),
                                 policy,ROOT,wait=False,label='DT framebuffer smoke')
    args.output.mkdir(parents=True,exist_ok=True);output=args.output.resolve()
    with tempfile.TemporaryDirectory(prefix='a20-dtfb-') as name:
        stage=Path(name);raw=stage/'qemu.dtb';patched=output/'firmware.dtb'
        base=[qemu,'-machine',cfg['machine'],'-cpu',cfg['cpu'],'-m',str(cfg['memory_mb']),
              '-smp','1','-device','ramfb','-display','none','-monitor','none','-net','none']
        dump=base.copy();dump[2]+=f',dumpdtb={raw}'
        subprocess.run(dump+['-serial','null'],check=True,capture_output=True)
        patched.write_bytes(inject(raw.read_bytes(),cfg))
        qmp_path,gdb_path=stage/'qmp',stage/'gdb'
        command=base+['-kernel',str(args.kernel.resolve()),'-dtb',str(patched),
                      '-serial','stdio','-S','-gdb',f'unix:{gdb_path},server=on,wait=off',
                      '-qmp',f'unix:{qmp_path},server=on,wait=off','-no-reboot']
        process=None
        with (output/'serial.log').open('wb') as log:
            try:
                process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT)
                deadline=time.monotonic()+args.timeout
                while not gdb_path.exists() and time.monotonic()<deadline:
                    if process.poll() is not None:raise ValueError('QEMU setup failed')
                    time.sleep(.05)
                debugger=Debugger(gdb_path);debugger.scanout(cfg);debugger.command('c');debugger.socket.close()
                def wait(marker):
                    while time.monotonic()<deadline:
                        text=(output/'serial.log').read_text(errors='replace')
                        if 'PANIC' in text or process.poll() is not None:raise ValueError(f'guest stopped before {marker}')
                        if marker in text:return
                        time.sleep(.1)
                    raise ValueError(f'timeout: {marker}')
                wait('[init] fork=0, calling execve mksh')
                process.stdin.write(b'videoplay --test 320 240 30 && echo DT_VIDEO_DONE\n');process.stdin.flush()
                wait('[FB] fb_ioctl: req=0x4609')
                with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
                    connection.settimeout(5);connection.connect(str(qmp_path))
                    stream=connection.makefile('rwb',buffering=0);json.loads(stream.readline())
                    def qmp(execute,arguments=None):
                        message={'execute':execute}
                        if arguments:message['arguments']=arguments
                        stream.write((json.dumps(message)+'\n').encode())
                        while True:
                            answer=json.loads(stream.readline())
                            if 'error'in answer:raise ValueError(str(answer))
                            if 'return'in answer:return
                    qmp('qmp_capabilities');time.sleep(.3)
                    shot=output/'screen.ppm';qmp('screendump',{'filename':str(shot)})
                    verify_bars(shot)
                wait('videoplay: presented 300 frames');wait('\nDT_VIDEO_DONE\n')
                process.stdin.write(b'poweroff\n');process.stdin.flush()
                process.wait(timeout=max(1,deadline-time.monotonic()))
                if process.returncode:raise ValueError(f'guest exit={process.returncode}')
            except (ValueError,OSError,subprocess.TimeoutExpired) as error:
                print(f'DT framebuffer FAIL: {error}; {output}');return 1
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
    print(f'DT framebuffer PASS: firmware DT, reserved WB RAM, userspace 300 frames, actual scanout pixels; {output}')
    return 0
if __name__=='__main__':raise SystemExit(main())
