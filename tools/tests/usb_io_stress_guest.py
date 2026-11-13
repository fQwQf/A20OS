#!/usr/bin/python3
"""Disposable guest workload: raw USB reads while the host injects HID reports."""
import os
import sys
import time
fd=os.open(sys.argv[1],os.O_RDONLY)
try:
    print('USB_IO_STRESS: READY',flush=True)
    time.sleep(2)
    until=time.monotonic()+20
    count=0
    while count<4096 or time.monotonic()<until:
        offset=1048576+((count*3571)%16384)*4096
        data=os.pread(fd,8192,offset)
        if len(data)!=8192:raise RuntimeError(f'short raw USB read: {len(data)}')
        count+=1
    print(f'USB_IO_STRESS: PASS reads={count} bytes_per_read=8192',flush=True)
finally:os.close(fd)
