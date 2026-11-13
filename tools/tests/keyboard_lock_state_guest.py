"""Observe compositor-driven LED state across press/release of actual USB keys."""
import fcntl,glob,json,os,struct,time
fd=None
for name in glob.glob('/dev/input/event*'):
 candidate=os.open(name,os.O_RDWR|os.O_NONBLOCK)
 ident=bytearray(8);fcntl.ioctl(candidate,0x80084502,ident)
 bits=bytearray(128);fcntl.ioctl(candidate,0x80804531,bits)
 if struct.unpack('HHHH',ident)[0]==3 and bits[0]&3==3:fd=candidate;break
 os.close(candidate)
assert fd is not None,'USB keyboard LEDs missing'
def state():
 value=bytearray(8);fcntl.ioctl(fd,0x80084519,value);return value[0]&3
original=state();start=time.monotonic();changes=[(0,original)]
print('LOCK_KEYS: READY',flush=True)
while time.monotonic()-start<7:
 value=state();elapsed=time.monotonic()-start
 if value!=changes[-1][1]:changes.append((elapsed,value))
 time.sleep(.005)
changes.append((time.monotonic()-start,state()));os.close(fd)
longest=[0.0,0.0]
for bit in (0,1):
 active=None
 for elapsed,value in changes:
  if (value^original)&(1<<bit):
   if active is None:active=elapsed
  elif active is not None:
   longest[bit]=max(longest[bit],elapsed-active);active=None
 if active is not None:longest[bit]=max(longest[bit],changes[-1][0]-active)
print('LOCK_KEYS: '+json.dumps(dict(changes=changes,seconds_inverted=longest,original=original)),flush=True)
assert changes[-1][1]==original and all(x>=.7 for x in longest),(changes,longest)
print('LOCK_KEYS: PASS stable CapsLock/NumLock after key release',flush=True)
