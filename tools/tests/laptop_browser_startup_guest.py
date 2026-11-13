#!/usr/bin/python3
"""Compare cold/warm Firefox startup within an isolated native test guest."""
import glob,importlib.util,json,os,runpy,shlex,socket,subprocess,time,urllib.request
HOST=os.environ.get('A20_TEST_HOST','http://10.0.2.2:38778')
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
urllib.request.urlretrieve(HOST+'/xfce-firefox-webdriver.py','/tmp/platform-webdriver.py')
webdriver=load('platform_webdriver','/tmp/platform-webdriver.py')
# Compare the normal userspace clock with an explicit kernel syscall while
# moving across every allowed CPU. HPET must also disable the raw-TSC vDSO.
import ctypes,struct
class Timespec(ctypes.Structure):
 _fields_=[('sec',ctypes.c_long),('nsec',ctypes.c_long)]
libc=ctypes.CDLL(None,use_errno=True)
allowed=os.sched_getaffinity(0)
previous_user=previous_kernel=0
reversals=0;maximum_gap=0
for repeat in range(10):
 for cpu in sorted(allowed):
  os.sched_setaffinity(0,{cpu})
  user=time.clock_gettime_ns(time.CLOCK_MONOTONIC)
  kernel=Timespec();assert libc.syscall(228,1,ctypes.byref(kernel))==0
  kernel_ns=kernel.sec*1000000000+kernel.nsec
  reversals+=int(user<previous_user or kernel_ns<previous_kernel)
  maximum_gap=max(maximum_gap,abs(user-kernel_ns))
  previous_user,previous_kernel=user,kernel_ns
os.sched_setaffinity(0,allowed)
aux=open('/proc/self/auxv','rb').read()
assert not any(k==33 and v for k,v in struct.iter_unpack('QQ',aux)),'raw TSC vDSO still exposed'
assert not reversals,reversals
print('PLATFORM_CLOCK: PASS cross-CPU monotonic clocks; raw TSC vDSO absent; max call gap_ns='+str(maximum_gap),flush=True)
subprocess.run(['/a20/rtw89d','--self-test'],check=True)
subprocess.run(['udhcpc','-n','-q','-t','3','-O','114','-i','eth0','-s','/a20/wifi-dhcp'],check=True)
assert urllib.request.urlopen(HOST+'/ok',timeout=15).read()==b'fixture ok'
print('PLATFORM_NETWORK: PASS DHCP and real HTTP through QEMU Ethernet',flush=True)
def active(pid):
 try:return open('/proc/%d/stat'%pid).read().rsplit(')',1)[1].split()[0] not in ('Z','X')
 except FileNotFoundError:return False
def firefox_tasks():
 found=set()
 for path in glob.glob('/proc/[0-9]*/exe'):
  try:
   exe=os.readlink(path);pid=int(path.split('/')[2])
   if '/firefox/' in exe and os.path.basename(exe).startswith('firefox') and active(pid):found.add(pid)
  except OSError:pass
 return found
def listening():
 sock=socket.socket();sock.settimeout(1)
 try:return sock.connect_ex(('127.0.0.1',2828))==0
 finally:sock.close()
results=[];seen_pids=set()
for lap in ('cold','warm'):
 profile='/tmp/platform-firefox-'+lap
 os.makedirs(profile,exist_ok=False)
 command=['/usr/lib/firefox/firefox','--marionette','--no-remote','--profile',profile,'about:blank']
 assert not listening(),'old Marionette listener still active'
 before=firefox_tasks()
 start=time.monotonic()
 subprocess.Popen(['/bin/sh','-c',' '.join(map(shlex.quote,command))+' >/tmp/platform-firefox-'+lap+'.log 2>&1 </dev/null &'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 browser=webdriver.Firefox()
 session=browser.command('WebDriver:NewSession',acceptInsecureCerts=False,pageLoadStrategy='eager')
 browser.command('WebDriver:Navigate',url='data:text/html,<title>A20 startup</title><h1 id="ready">Ready</h1>')
 browser.wait_script("return document.querySelector('#ready') !== null;")
 seconds=time.monotonic()-start
 pid=session['capabilities']['moz:processID']
 assert pid not in seen_pids,'attached to a previous Firefox process'
 seen_pids.add(pid)
 result={'lap':lap,'seconds_to_DOM':seconds,'browser_pid':pid}
 results.append(result);print('PLATFORM_BROWSER: '+json.dumps(result),flush=True)
 owned=firefox_tasks()-before
 assert pid in owned,('Firefox process inventory missing',pid,owned)
 # Use Firefox's own shutdown path, rather than treating one dead Linux
 # thread as proof that every browser window and worker exited.
 quit_result=browser.command('Marionette:Quit',flags=['eForceQuit'])
 assert quit_result['cause']=='shutdown',quit_result
 browser.stream.close();browser.sock.close()
 deadline=time.monotonic()+60
 while (any(active(p) for p in owned) or listening()) and time.monotonic()<deadline:time.sleep(.2)
 remaining=[p for p in owned if active(p)]
 assert not remaining and not listening(),('Firefox still active',remaining)
 print('PLATFORM_BROWSER_EXIT: PASS pid='+str(pid)+' tasks='+str(len(owned)),flush=True)
 time.sleep(2)
print('PLATFORM_BROWSER: PASS cold/warm DOM navigation; timing is QEMU virtual storage only',flush=True)
urllib.request.urlretrieve(HOST+'/laptop_platform_guest.py','/tmp/platform-leds.py')
runpy.run_path('/tmp/platform-leds.py',run_name='__main__')
