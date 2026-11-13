"""Desktop power actions delegate to OpenRC; a rejected request stays visible."""
import contextlib
import importlib.util
import io
import subprocess
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class SessionPower(unittest.TestCase):
    def test_upstream_init_requests_and_refusal(self):
        messages = []

        class Dialog:
            def __init__(self, **kw):
                messages.append(kw['text'])
            def format_secondary_text(self, text):
                messages.append(text)
            def connect(self, *args):
                pass
            def show_all(self):
                messages.append('visible')

        gi = types.ModuleType('gi')
        gi.require_version = lambda *args: None
        repository = types.ModuleType('gi.repository')
        repository.Gio = types.SimpleNamespace()
        repository.GLib = types.SimpleNamespace(SOURCE_REMOVE=False)
        repository.Gtk = types.SimpleNamespace(
            MessageDialog=Dialog, MessageType=types.SimpleNamespace(ERROR=1),
            ButtonsType=types.SimpleNamespace(CLOSE=1))
        spec = importlib.util.spec_from_file_location(
            'test_power_provider', ROOT / 'packages/overlay/xfce/usr/lib/a20/session-power.py')
        module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {'gi': gi, 'gi.repository': repository}):
            spec.loader.exec_module(module)
            for action in (module.INIT_RESTART, module.INIT_POWER_OFF):
                result = types.SimpleNamespace(returncode=0, stdout='')
                with patch.object(module.subprocess, 'run', return_value=result) as run, \
                     patch.object(module.os, 'sync', side_effect=AssertionError('init owns sync')), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertFalse(module.power_action(action))
                    self.assertEqual(run.call_args.args[0], ['/sbin/' + action])
                    self.assertEqual(run.call_args.kwargs['timeout'], 15)
                    self.assertNotIn('shell', run.call_args.kwargs)
            self.assertNotIn('visible', messages)
            failure = types.SimpleNamespace(returncode=1, stdout='init control FIFO unavailable')
            for result, error in ((failure, None), (None, OSError('missing init tool')),
                                  (None, subprocess.TimeoutExpired('openrc-shutdown', 15))):
                with patch.object(module.subprocess, 'run', return_value=result, side_effect=error), \
                     patch.object(module.os, '_exit') as terminate, \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertFalse(module.power_action(module.INIT_RESTART))
                    terminate.assert_not_called()
            self.assertEqual(messages.count('visible'), 3)
            self.assertIn('init control FIFO unavailable', messages)
            with self.assertRaises(ValueError):
                module.power_action('-f')


if __name__ == '__main__':
    unittest.main()
