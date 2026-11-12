"""A refused power request must leave the session provider alive and explain why."""
import contextlib,ctypes,errno,importlib.util,io,sys,types,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
class SessionPower(unittest.TestCase):
 def test_refusal_keeps_provider_alive(self):
  messages=[]
  class Dialog:
   def __init__(self,**kw):messages.append(kw['text'])
   def format_secondary_text(self,text):messages.append(text)
   def connect(self,*a):pass
   def show_all(self):messages.append('visible')
  gi=types.ModuleType('gi');gi.require_version=lambda *a:None
  repository=types.ModuleType('gi.repository')
  repository.Gio=types.SimpleNamespace()
  repository.GLib=types.SimpleNamespace(SOURCE_REMOVE=False)
  repository.Gtk=types.SimpleNamespace(MessageDialog=Dialog,MessageType=types.SimpleNamespace(ERROR=1),ButtonsType=types.SimpleNamespace(CLOSE=1))
  spec=importlib.util.spec_from_file_location('test_power_provider',ROOT/'packages/overlay/xfce/usr/lib/a20/session-power.py')
  module=importlib.util.module_from_spec(spec)
  with patch.dict(sys.modules,{'gi':gi,'gi.repository':repository}):
   spec.loader.exec_module(module)
   for error,expected in ((errno.EBUSY,20),(errno.EIO,1)):
    calls=[]
    def reboot(cmd):calls.append(cmd);ctypes.set_errno(error);return -1
    with patch.object(module.ctypes,'CDLL',return_value=types.SimpleNamespace(reboot=reboot)),patch.object(module.os,'sync',side_effect=AssertionError('duplicate global sync')),patch.object(module.os,'_exit') as terminate,patch('time.sleep'),contextlib.redirect_stdout(io.StringIO()):
     self.assertFalse(module.power_action(module.LINUX_REBOOT_CMD_POWER_OFF))
     terminate.assert_not_called();self.assertEqual(len(calls),expected)
   self.assertEqual(messages.count('visible'),2)
if __name__=='__main__':unittest.main()
