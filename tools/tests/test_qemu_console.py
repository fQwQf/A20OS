"""The real console relay must preserve bytes while its consumer stalls."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[2]
FAKE=r'''
import socket,sys
from pathlib import Path
paths={}
for value in sys.argv:
 if value.startswith('socket,'):
  opts=dict(item.split('=',1) for item in value.split(',')[1:] if '=' in item)
  paths[opts['id']]=opts['path']
u=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);u.bind(paths['a20uart']);u.listen()
uart,_=u.accept()
t=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);t.bind(paths['a20terminal']);t.listen()
terminal,_=t.accept()
data=b''
while len(data)<3:data+=uart.recv(3-len(data))
assert data==b'ab\x03',repr(data)
uart.sendall(b'BOOT:'+data+b'\n')
block=bytes(range(256))*256
for i in range(128):terminal.sendall(block)
Path(sys.argv[1]).write_text('sent')
terminal.close();uart.close();t.close();u.close()
'''


class QemuConsoleRelayTests(unittest.TestCase):
    def test_qemu_exit_chord_terminates_the_owned_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake=Path(tmp)/'fake.py';fake.write_text(FAKE)
            result=subprocess.run([sys.executable,str(ROOT/'tools/qemu_console.py'),'--',
                sys.executable,str(fake),str(Path(tmp)/'sent')],input=b'\x01x',
                capture_output=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr.decode())

    def test_bounded_backpressure_and_input_signal_forwarding(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake=Path(tmp)/'fake.py';fake.write_text(FAKE);marker=Path(tmp)/'sent'
            process=subprocess.Popen([sys.executable,str(ROOT/'tools/qemu_console.py'),'--',
                sys.executable,str(fake),str(marker)],stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                process.stdin.write(b'ab\x03');process.stdin.flush()
                time.sleep(.25)
                self.assertFalse(marker.exists(),'relay buffered the entire stream instead of applying backpressure')
                output,errors=process.communicate(timeout=15)
                self.assertEqual(process.returncode,0,errors.decode())
                self.assertEqual(output,b'BOOT:ab\x03\n'+bytes(range(256))*256*128)
                self.assertTrue(marker.exists())
            finally:
                if process.poll() is None:process.kill();process.communicate()


if __name__=='__main__':
    unittest.main()
