#!/usr/bin/env python3
"""Bounded terminal relay for the early UART and a flow-controlled virtio port.

Use independent socket backends: QEMU's stdio mux has no writable-watch
forwarding, which a throttled virtserialport needs. Keep at most 128 KiB of
output buffered and stop reading sockets when stdout applies backpressure.
"""
from __future__ import annotations
import fcntl
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import time
import tty

OUTPUT_LIMIT = 128 * 1024
INPUT_LIMIT = 16 * 1024


def console_argv(argv, directory):
    return [*argv, '-chardev', f'socket,id=a20uart,path={directory}/uart,server=on,wait=on',
            '-serial', 'chardev:a20uart', '-chardev',
            f'socket,id=a20terminal,path={directory}/terminal,server=on,wait=on',
            '-device', 'virtio-serial-pci,id=a20tty', '-device',
            'virtserialport,chardev=a20terminal,bus=a20tty.0,nr=1']


def connect(path, process):
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        try:
            sock.connect(str(path));sock.setblocking(False);return sock
        except (FileNotFoundError,ConnectionRefusedError):
            sock.close()
            if process.poll() is not None:
                raise RuntimeError('QEMU exited while opening console sockets')
            time.sleep(.01)
    raise TimeoutError('QEMU did not open console socket '+str(path))


def relay(process, uart, terminal):
    incoming=bytearray();outgoing=bytearray();sockets=[uart,terminal]
    stdin_open=True;escape=False
    while sockets or process.poll() is None:
        reads=list(sockets) if len(outgoing)<OUTPUT_LIMIT else []
        if stdin_open and len(incoming)<INPUT_LIMIT and uart in sockets:
            reads.append(0)
        writes=[]
        if outgoing:writes.append(1)
        if incoming and uart in sockets:writes.append(uart)
        if not reads and not writes:
            break
        readable,writable,_=select.select(reads,writes,[],.1)
        for source in readable:
            if source != 0 and len(outgoing) >= OUTPUT_LIMIT:
                continue
            try:
                data=os.read(source if isinstance(source,int) else source.fileno(),
                             min(4096,INPUT_LIMIT-len(incoming)) if source==0
                             else min(65536,OUTPUT_LIMIT-len(outgoing)))
            except BlockingIOError:
                continue
            if not data:
                if source==0:stdin_open=False
                else:sockets.remove(source)
                continue
            if source==0:
                # Preserve the familiar QEMU exit chord while forwarding
                # Ctrl-C as a guest TTY byte, never as a host SIGINT.
                for byte in data:
                    if escape:
                        escape=False
                        if byte==ord('x'):
                            process.terminate();return True
                        incoming.append(1)
                        if byte==1:continue
                    elif byte==1:
                        escape=True;continue
                    incoming.append(byte)
            else:
                outgoing.extend(data)
        if uart in writable:
            try:
                sent=uart.send(incoming);del incoming[:sent]
            except BlockingIOError:
                pass
        if 1 in writable:
            try:
                sent=os.write(1,outgoing);del outgoing[:sent]
            except BlockingIOError:
                pass
        if process.poll() is not None and not sockets:
            while outgoing:
                try:
                    sent=os.write(1,outgoing);del outgoing[:sent]
                except BlockingIOError:
                    select.select([],[1],[],.1)
            break
    return False


def main():
    if len(sys.argv)<3 or sys.argv[1]!='--':
        raise SystemExit('usage: qemu_console.py -- QEMU [arguments...]')
    old_tty=termios.tcgetattr(0) if os.isatty(0) else None
    flags={fd:fcntl.fcntl(fd,fcntl.F_GETFL) for fd in (0,1)}
    process=None;uart=terminal=None
    def stop(signum, frame):
        raise SystemExit(128+signum)
    for signum in (signal.SIGTERM,signal.SIGHUP):signal.signal(signum,stop)
    try:
        if old_tty:
            tty.setraw(0,termios.TCSANOW)
            settings=termios.tcgetattr(0)
            settings[1]=old_tty[1]  # retain the operator's normal output processing
            termios.tcsetattr(0,termios.TCSANOW,settings)
        for fd in (0,1):fcntl.fcntl(fd,fcntl.F_SETFL,flags[fd]|os.O_NONBLOCK)
        with tempfile.TemporaryDirectory(prefix='a20-console-') as directory:
            process=subprocess.Popen(console_argv(sys.argv[2:],directory),stdin=subprocess.DEVNULL)
            uart=connect(Path(directory)/'uart',process)
            terminal=connect(Path(directory)/'terminal',process)
            requested_exit=False
            try:
                requested_exit=relay(process,uart,terminal)
            except (BrokenPipeError,ConnectionResetError):
                pass
            if process.poll() is None:
                try:process.wait(timeout=.5)
                except subprocess.TimeoutExpired:process.terminate()
            status=process.wait(timeout=5)
            return 0 if requested_exit else status
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        if uart:uart.close()
        if terminal:terminal.close()
        for fd in (0,1):fcntl.fcntl(fd,fcntl.F_SETFL,flags[fd])
        if old_tty:termios.tcsetattr(0,termios.TCSANOW,old_tty)


if __name__=='__main__':
    raise SystemExit(main())
