#!/usr/bin/python3
"""Native guest LED output/readback and a deliberately sleeping pipe reader."""
import fcntl,glob,os,struct,subprocess,time
EV_LED=0x11
EVIOCGID=0x80084502
led_query=0x80084519
format='llHHi'
def owners():
 found={}
 for p in glob.glob('/proc/[0-9]*/comm'):
  try:
   name=open(p).read().strip()
   if name in ('labwc','xfdesktop','xfce4-panel'):found[name]=int(p.split('/')[2])
  except OSError:pass
 return found
before=owners();assert all(n in before for n in ('labwc','xfdesktop','xfce4-panel')),before
found=False
for path in sorted(glob.glob('/dev/input/event*')):
 fd=os.open(path,os.O_RDWR|os.O_NONBLOCK)
 try:
  ident=bytearray(8);fcntl.ioctl(fd,EVIOCGID,ident)
  if struct.unpack('HHHH',ident)[0]!=3:continue
  bits=bytearray(128);fcntl.ioctl(fd,0x80804520|EV_LED,bits)
  if bits[0]&3!=3:continue
  original=bytearray(8);fcntl.ioctl(fd,led_query,original)
  previous=original[0]&3
  for target in (3,0,1,2,previous):
   output=b''.join(struct.pack(format,0,0,EV_LED,bit,int(bool(target&(1<<bit)))) for bit in (0,1))
   output+=struct.pack(format,0,0,0,0,0)
   assert os.write(fd,output)==len(output)
   actual=bytearray(8);fcntl.ioctl(fd,led_query,actual)
   assert actual[0]&3==target,(target,actual[0])
  print('PLATFORM_LEDS: PASS USB output and EVIOCGLED readback',flush=True);found=True;break
 finally:os.close(fd)
assert found,'no USB keyboard advertising Num/Caps LEDs'
r,w=os.pipe()
reader=os.fork()
if reader==0:
 os.close(w);os.read(r,1);os._exit(0)
os.close(r)
# Keep w alive in a separate sleeping process even after this fixture exits.
holder=os.fork()
if holder==0:
 time.sleep(300);os._exit(0)
os.close(w)
time.sleep(.3)
assert owners()==before,(before,owners())
# Keep the actual graphical-session D-Bus request in a guest file so the
# harness can type a short command after explicitly confirming terminal focus.
with open('/tmp/a20-gui-poweroff','w') as command:
 command.write('exec dbus-send --session --type=method_call --dest=org.xfce.SessionManager /org/xfce/SessionManager org.xfce.Session.Manager.Shutdown boolean:false\n')
print('PLATFORM: PASS keyboard LEDs; blocked pipe reader remains for power test',flush=True)
