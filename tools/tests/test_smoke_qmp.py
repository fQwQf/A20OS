"""Exercise the production QMP handshake/pumps against a real Unix socket peer."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import select
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch
from tools import smoke
from tools.tests.check_usb_hotplug_log import check


class SmokeQmp(unittest.TestCase):
    def serve(self, pump, commands, reject=False):
        self.acknowledged = threading.Event()
        with tempfile.TemporaryDirectory(prefix='a20-qmp-') as tmp:
            path = str(Path(tmp) / 'qmp.sock')
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(path)
                listener.listen(1)
                listener.settimeout(3)

                def peer():
                    with listener.accept()[0] as sock:
                        sock.settimeout(3)
                        with sock.makefile('rwb') as stream:
                            def send(value):
                                stream.write((json.dumps(value) + '\r\n').encode())
                                stream.flush()

                            send({'QMP': {'version': {}, 'capabilities': []}})
                            capability = json.loads(stream.readline())
                            self.assertEqual(capability['execute'], 'qmp_capabilities')
                            if reject:
                                send({'error': {'class': 'GenericError'}, 'id': capability.get('id')})
                                self.assertEqual(stream.readline(), b'')
                                return
                            send({'event': 'RESET'})
                            send({'return': {}, 'id': capability.get('id')})
                            for index, command in enumerate(commands):
                                request = json.loads(stream.readline())
                                self.assertEqual(request['execute'], command)
                                self.assertNotEqual(request.get('id'), capability.get('id'))
                                send({'event': 'DEVICE_DELETED'})
                                # An unrelated reply may not complete the command.
                                send({'return': {'wrong': True}, 'id': 'unrelated'})
                                if index == len(commands) - 1:
                                    ready, _, _ = select.select([sock], [], [], .02)
                                    self.assertFalse(ready, 'client closed before the final reply')
                                    self.acknowledged.set()
                                send({'return': {}, 'id': request.get('id')})
                            self.assertEqual(stream.readline(), b'')

                with ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(peer)
                    pump(path)
                    future.result(timeout=4)

    def test_actions_wait_for_handshake_and_final_matching_reply(self):
        self.serve(lambda path: smoke.qmp_action_pump(path, [
            {'execute': 'device_del'}, {'execute': 'device_add'}]),
            ['device_del', 'device_add'])

    def test_key_pump_uses_the_same_capability_handshake(self):
        def pump(path):
            with patch.object(smoke.time, 'time', side_effect=lambda: 2 if self.acknowledged.is_set() else 0):
                smoke.qmp_key_pump(path, ['a'], 0, 0, 1)
        self.serve(pump, ['input-send-event', 'input-send-event'])

    def test_rejected_capabilities_close_the_connection(self):
        self.serve(lambda path: self.assertEqual(smoke.qmp_connect(path), (None, None)),
                   [], reject=True)

    def test_live_hotplug_checker_excludes_replayed_initial_enumeration(self):
        initial = '[USB-HID] keyboard ready\n'
        removed = '[USB] port 5 disconnected: address=1 reclaimed, slot=1 released\n'
        replug = '[USB] port 5 slot=1 address=1\n' + initial
        replay = '===== A20OS BOOT REPORT =====\n' + replug + '===== END BOOT REPORT =====\n'
        with self.assertRaises(ValueError):
            check(initial + removed + replay)
        check(initial + removed + replay + replug)
        with self.assertRaises(ValueError):
            check(initial + replug + removed)

    def test_busy_guest_slot_cannot_certify_old_logs_or_run_post_checks(self):
        case = {'log': '/nonexistent-qmp-test-log', 'argv': [], 'expect': ['PASS'], 'pass_msg': 'PASS'}
        self.assertEqual(smoke.report('busy', case, 75), 75)
        with patch.dict(smoke.CASES, {'busy-test': case}), \
             patch.object(smoke, 'resolve_case_build_paths', side_effect=dict), \
             patch.object(smoke, 'run_gate'), patch.object(smoke, 'run_build'), \
             patch.object(smoke, 'run_post_build', return_value=0), \
             patch.object(smoke, 'run_qemu', return_value=75), \
             patch.object(smoke, 'run_post') as post:
            self.assertEqual(smoke.main(['busy-test']), 75)
            post.assert_not_called()
