"""Radio framing and captive-portal behavior; no wireless hardware required."""
import http.server
import importlib.util
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "user/wifi"))
import wifi_backend as backend
import wifi_portal as portal


class PortalTest(unittest.TestCase):
    def check(self, http=(204, {}, b""), https=(204, {}, b"")):
        def request(url, accept="*/*"):
            value = https if url.startswith("https:") else http
            if isinstance(value, Exception):
                raise value
            return value
        return portal.detect("http://probe.test/204", "https://probe.test/204", request=request)

    def test_https_required_for_online(self):
        self.assertEqual(self.check()["state"], "online")
        for failure in (OSError("DNS"), ssl.SSLCertVerificationError("certificate"),
                        (302, {"Location": "http://login.test/"}, b""), (200, {}, b"login")):
            with self.subTest(failure=failure):
                self.assertEqual(self.check(https=failure)["state"], "unknown")

    def test_redirect_and_html_are_captive(self):
        result = self.check(http=(302, {"Location": "/portal?token=abc"}, b""))
        self.assertEqual(result["state"], "portal")
        self.assertEqual(result["login_url"], "http://probe.test/portal?token=abc")
        self.assertEqual(self.check(http=(200, {}, b"<script>location='/login'</script>"))["state"], "portal")
        self.assertEqual(self.check(http=(503, {}, b"unavailable"))["state"], "unknown")

    def test_unsafe_urls_never_launch(self):
        for url in ("file:///etc/passwd", "javascript:alert(1)", "https://u:p@host/",
                    "http://host/\n--flag", "https://[invalid", "https://host:bad/"):
            self.assertIsNone(portal.safe_url(url))
        self.assertEqual(portal.safe_url("http://10.1.2.3:8080/login?a=1"),
                         "http://10.1.2.3:8080/login?a=1")
        self.assertIsNone(portal.safe_url("http://login/", https_only=True))

    def test_capport_uri_and_api(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "option114"
            uri = "https://portal.test/api"
            for value in (uri, uri.encode().hex()):
                path.write_text(value)
                self.assertEqual(portal.capport_uri(path), uri)
            path.write_text("687474703a2f2f686f7374")
            self.assertIsNone(portal.capport_uri(path))
        def request(url, accept="*/*"):
            return 200, {}, json.dumps({"captive": True,
                                       "user-portal-url": "https://portal.test/signin"}).encode()
        result = portal.detect(api=uri, request=request)
        self.assertEqual(result["state"], "portal")
        self.assertEqual(result["login_url"], "https://portal.test/signin")

    def test_fetch_preserves_redirect_without_following(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(302)
                self.send_header("Location", "/signin")
                self.end_headers()
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            code, headers, body = portal.fetch(f"http://127.0.0.1:{server.server_port}/probe")
            self.assertEqual(code, 302)
            self.assertEqual(headers["Location"], "/signin")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_untrusted_https_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            cert, key = Path(directory)/"cert.pem", Path(directory)/"key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(key), "-out", str(cert), "-days", "1",
                            "-subj", "/CN=localhost"], check=True, capture_output=True)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), http.server.BaseHTTPRequestHandler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with self.assertRaises(OSError):
                    portal.fetch(f"https://127.0.0.1:{server.server_port}/")
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


class RadioTest(unittest.TestCase):
    def test_missing_radio_has_gui_diagnostic_hint(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(OSError, "诊断日志"):
                backend.Radio(str(Path(directory) / "missing")).status()

    def test_fragmented_protocol_and_exact_ssids(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "radio")
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(path)
            server.listen(1)
            captured = []
            ssid = b"name'\n\x00end"
            def serve():
                for _ in range(2):
                    with server.accept()[0] as stream:
                        command = struct.unpack("<IIII32s64s", backend.receive(stream, 112))
                        captured.append(command)
                        body = json.dumps([
                            {"ssid_hex": ssid.hex(), "signal": -50, "frequency": 2412, "security": 0},
                            {"ssid_hex": b"WHU-WLAN".hex(), "signal": -80, "frequency": 5180, "security": 0},
                            {"ssid_hex": ssid.hex(), "signal": -70, "frequency": 2437, "security": 0},
                        ]).encode() if command[1] == backend.SCAN_JSON else b"authorized=1 ip=10.1.2.3"
                        reply = struct.pack("<iI", 0, len(body)) + body
                        for offset in range(0, len(reply), 3):
                            stream.sendall(reply[offset:offset+3])
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            try:
                radio = backend.Radio(path)
                networks = radio.scan()
                self.assertEqual(len(networks), 2)
                self.assertEqual(networks[0]["ssid"], ssid)
                self.assertEqual(networks[1]["ssid"], b"WHU-WLAN")
                self.assertEqual(networks[0]["signal"], -50)
                radio.connect(ssid)
                self.assertEqual(captured[1][2:4], (len(ssid), 0))
                self.assertEqual(captured[1][4][:len(ssid)], ssid)
                self.assertEqual(captured[1][5], b"\0" * 64)
            finally:
                thread.join(3)
                server.close()
            self.assertFalse(thread.is_alive())


if __name__ == "__main__":
    unittest.main()
