#!/usr/bin/env python3
"""Test the real Wi-Fi GTK window and Firefox against a captive portal fixture.

Only the radio association is simulated: QEMU does not emulate RTL8852BE.
The HTTP redirect, JS form, cookies, browser and online recheck run in A20OS.
"""
import argparse
import http.server
from pathlib import Path
import subprocess
import sys
import threading
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]


class Portal(http.server.BaseHTTPRequestHandler):
    authenticated = False
    def reply(self, code, body=b"", **headers):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("Content-Type", "text/html; charset=utf-8")
        for name, value in headers.items():
            self.send_header(name.replace("_", "-"), value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/fixture.py":
            fixture = (ROOT / "tools/tests/wifi_gui_guest.py").read_text()
            self.reply(200, fixture.replace("@HOST@", f"http://10.0.2.2:{self.server.server_port}").encode())
        elif path == "/usb-io-stress.py":
            self.reply(200, (ROOT / "tools/tests/usb_io_stress_guest.py").read_bytes())
        elif path in ("/input-method.py", "/im"):
            self.reply(200, (ROOT / "tools/tests/memfd_churn_guest.py").read_bytes() + b'\n' +
                       (ROOT / "tools/tests/accept_flags_guest.py").read_bytes() + b'\n' +
                       (ROOT / "tools/tests/orphan_survival_guest.py").read_bytes() + b'\n' +
                       (ROOT / "tools/tests/dconf_cold_guest.py").read_bytes() + b'\n' +
                       (ROOT / "tools/tests/input_method_guest.py").read_bytes())
        elif path == "/webdriver.py":
            self.reply(200, (ROOT / "tools/tests/xfce-firefox-webdriver.py").read_bytes())
        elif path == "/probe":
            self.reply(204) if self.authenticated else self.reply(302, Location="/login")
        elif path == "/login":
            self.reply(200, b'''<!doctype html><meta charset="utf-8"><title>Campus sign-in test</title>
<h1>Campus sign-in test</h1><form id="login" method="post" action="/authorize">
<label>Account <input id="account" name="account"></label><br>
<label>Password <input id="password" name="password" type="password"></label>
<input id="challenge" name="challenge" type="hidden"><button>Sign in</button></form>
<script>document.querySelector('#login').addEventListener('submit', function () {
document.querySelector('#challenge').value = String(6 * 7); });</script>''')
        elif path == "/complete" and "session=fixture-ok" in self.headers.get("Cookie", ""):
            self.reply(200, b'<h1 id="complete">Authenticated</h1>')
        else:
            self.reply(403, b"Not authenticated")

    def do_POST(self):
        data = urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode())
        if self.path == "/authorize" and data == {"account": ["fixture-user"],
            "password": ["fixture-password"], "challenge": ["42"]}:
            type(self).authenticated = True
            self.reply(302, Location="/complete", Set_Cookie="session=fixture-ok; HttpOnly; Path=/")
        else:
            self.reply(403, b"Invalid fixture credentials or missing JavaScript challenge")

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--esp-log-image", type=Path, required=True)
    parser.add_argument("--input-method-check", action="store_true")
    parser.add_argument("--input-method-only", action="store_true",
                        help="focus on IBus/Rime, omitting USB stress and portal browser")
    parser.add_argument("--interactive-on-failure", action="store_true")
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Portal)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        return subprocess.call([sys.executable, str(ROOT / "tools/qemu/check_laptop_gui.py"),
            "--image", str(args.image), "--log", str(args.log), "--esp-log-image", str(args.esp_log_image),
            "--storage", "usb-hub", "--usb-keyboard", "--smp", "16", "--memory", "40G", "--memfd",
            *(["--interactive-on-failure"] if args.interactive_on_failure else []),
            "--min-memory-mib", "39800", "--wifi-selftest", "--wifi-dhcp-test", "--touchpad-check",
            *([] if args.input_method_only else ["--usb-io-stress-fixture",
              f"http://10.0.2.2:{server.server_port}/usb-io-stress.py"]),
            *(["--input-method-fixture", f"http://10.0.2.2:{server.server_port}/im"]
              if args.input_method_check or args.input_method_only else []),
            *([] if args.input_method_only else ["--wifi-gui-fixture",
              f"http://10.0.2.2:{server.server_port}/fixture.py"]), "--timeout", "240"])
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
