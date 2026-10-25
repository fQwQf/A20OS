"""Real localhost UDP transfers for the one-file read-only bundle server."""
from pathlib import Path
import hashlib
import json
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import a20_tftp


class BundleTFTP(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bundle = Path(self.temp.name)
        self.payload = bytes(range(256)) * 4  # Exact block boundary needs final empty DATA.
        (self.bundle / "kernel.bin").write_bytes(self.payload)
        (self.bundle / "manifest.json").write_text(json.dumps({"board": "licheerv-nano", "files": {
            "kernel.bin": {"size": len(self.payload), "sha256": hashlib.sha256(self.payload).hexdigest()}}}))

    def test_corrupt_or_symlink_payload_never_opens_a_socket(self):
        (self.bundle / "kernel.bin").write_bytes(bytes(len(self.payload)))
        with patch.object(a20_tftp, "_serve_bytes") as server:
            with self.assertRaisesRegex(ValueError, "hash differs"):
                a20_tftp.serve_tftp(self.bundle)
            server.assert_not_called()
        (self.bundle / "kernel.bin").unlink()
        (self.bundle / "kernel.bin").symlink_to("/dev/null")
        with self.assertRaisesRegex(ValueError, "non-symlink"):
            a20_tftp.verified_kernel(self.bundle)

    def test_invalid_record_and_size_are_rejected(self):
        manifest = self.bundle / "manifest.json"
        original = json.loads(manifest.read_text())
        for value in (True, -1, 0, a20_tftp.MAX_KERNEL_BYTES + 1, "1024"):
            original["files"]["kernel.bin"]["size"] = value
            manifest.write_text(json.dumps(original))
            with self.subTest(value=value), self.assertRaises(ValueError):
                a20_tftp.verified_kernel(self.bundle)

    def test_udp_rejects_writes_and_paths_then_serves_immutable_snapshot(self):
        ready, addresses = threading.Event(), []
        failures = []
        def on_ready(address):
            addresses.append(address)
            ready.set()
        def serve():
            try:
                a20_tftp.serve_tftp(self.bundle, "127.0.0.1", 0, once=True, on_ready=on_ready)
            except Exception as error:
                failures.append(error)
                ready.set()
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        self.assertTrue(ready.wait(2))
        self.assertFalse(failures, failures)
        # Repackaging after startup cannot change the verified snapshot.
        (self.bundle / "kernel.bin").write_bytes(b"new unverified bytes")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(2)
            for request in (b"\0\2kernel.bin\0octet\0", b"\0\1../kernel.bin\0octet\0",
                            b"\0\1/etc/passwd\0octet\0", b"\0\1kernel.bin\0netascii\0"):
                client.sendto(request, addresses[0])
                packet, _ = client.recvfrom(2048)
                self.assertEqual(packet[:2], b"\0\5")
            # Declining blksize options with DATA block 1 is RFC1350 fallback.
            client.sendto(b"\0\1kernel.bin\0octet\0blksize\01468\0", addresses[0])
            received, blocks = bytearray(), []
            while True:
                packet, transfer = client.recvfrom(2048)
                opcode, block = struct.unpack("!HH", packet[:4])
                self.assertEqual(opcode, 3)
                blocks.append(block)
                received.extend(packet[4:])
                client.sendto(struct.pack("!HH", 4, block), transfer)
                if len(packet[4:]) < 512:
                    break
            self.assertEqual(blocks, [1, 2, 3])
            self.assertEqual(received, self.payload)
        worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertFalse(failures)


if __name__ == "__main__":
    unittest.main()
