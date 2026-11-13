#!/usr/bin/python3
"""Disposable guest fixture; never included in the production image."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

HOST = "@HOST@"
sys.path.insert(0, "/a20")
from wifi_backend import Radio, receive
import wifi_portal

def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module

gui = load("wifi_gui", "/a20/wifi-gui")
Gtk, GLib = gui.Gtk, gui.GLib

def desktop_owners():
    owners = {}
    for name in ('labwc', 'xfdesktop', 'xfce4-panel'):
        leaders = set()
        for pid in subprocess.check_output(['pidof', name], text=True).split():
            try:
                tasks = [int(p.name) for p in Path('/proc/' + pid + '/task').iterdir()
                         if p.name.isdigit()]
            except FileNotFoundError:
                continue
            if tasks:
                # The bounded run cannot wrap PIDs; normalize pidof's task IDs.
                leaders.add(min(tasks))
        assert leaders, ('desktop process missing', name)
        owners[name] = leaders
    return owners

original_owners = desktop_owners()
desktop = Path("/usr/share/applications/a20-wifi.desktop").read_text()
assert "Exec=/a20/wifi-gui" in desktop and "Terminal=false" in desktop
assert Path("/root/Desktop/a20-wifi.desktop").is_file()
with open("/tmp/a20-wifi-launch.log", "wb") as launch_log:
    launcher = subprocess.Popen(["/a20/wifi-gui"], stdout=launch_log, stderr=launch_log)
    time.sleep(3)
    assert launcher.poll() is None, Path("/tmp/a20-wifi-launch.log").read_text()
    launcher.terminate()
    launcher.wait(timeout=5)
print("WIFI_GUI: production launcher and desktop entries PASS", flush=True)
os.environ["MOZ_MARIONETTE"] = "1"
path = "/tmp/a20-test-radio.sock"
Path(path).unlink(missing_ok=True)
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(4)
authorized = False
connects = 0

def radio_server():
    global authorized, connects
    while True:
        with server.accept()[0] as stream:
            command = struct.unpack("<IIII32s64s", receive(stream, 112))
            operation = command[1]
            if operation == 5:
                body = json.dumps([{"ssid_hex": b"WHU-WLAN".hex(), "signal": -42,
                                    "frequency": 5180, "security": 0}]).encode()
            elif operation == 3:
                assert command[2:4] == (8, 0), "open SSID must not request a password"
                assert command[4][:8] == b"WHU-WLAN"
                assert command[5] == b"\0" * 64
                connects += 1
                authorized = True
                body = b"authorized=1 ip=10.0.2.15"
            else:
                body = f"authorized={int(authorized)} ip={'10.0.2.15' if authorized else 'none'}".encode()
            stream.sendall(struct.pack("<iI", 0, len(body)) + body)

threading.Thread(target=radio_server, daemon=True).start()
app = Gtk.Application(application_id="org.a20os.Wifi.Test")
app.register(None)
window = gui.WifiWindow(app, radio=Radio(path),
                       detector=lambda: wifi_portal.detect(HOST + "/probe", wifi_portal.HTTPS_PROBE))
phase = 0
started = time.monotonic()
failure = None

def browser_test():
    global failure
    try:
        target = "/tmp/wifi-webdriver.py"
        urllib.request.urlretrieve(HOST + "/webdriver.py", target)
        webdriver = load("wifi_webdriver", target)
        browser = webdriver.Firefox()
        session = browser.command("WebDriver:NewSession", acceptInsecureCerts=False,
                                  pageLoadStrategy="eager")
        assert session['capabilities']['acceptInsecureCerts'] is False
        browser.command("WebDriver:SetTimeouts", pageLoad=60000, script=30000)
        browser.command("WebDriver:Navigate", url=HOST + "/probe")
        browser.wait_script("return document.querySelector('#login') !== null;")
        print("WIFI_GUI: browser redirected to portal", flush=True)
        browser.script("document.querySelector('#account').value='fixture-user';"
                       "document.querySelector('#password').value='fixture-password';"
                       "document.querySelector('#login').requestSubmit(); return true;")
        browser.wait_script("return document.querySelector('#complete') !== null;")
        assert browser.script("return document.querySelector('#complete').textContent;") == 'Authenticated'
        print("WIFI_GUI: Firefox JavaScript + form + cookie + redirect PASS", flush=True)
        GLib.idle_add(window.check_network)
    except Exception as error:
        failure = str(error)
        print("WIFI_GUI: FAIL browser: " + repr(error), flush=True)

def advance():
    global phase, started
    if failure:
        return False
    if phase == 0 and not window.busy and len(window.store):
        print("WIFI_GUI: READY desktop window", flush=True)
        phase, started = 1, time.monotonic()
    elif phase == 1 and time.monotonic() - started > 5:
        def close_diagnostic():
            for dialog in Gtk.Window.list_toplevels():
                if dialog.get_title() == "无线诊断日志":
                    dialog.response(Gtk.ResponseType.CLOSE)
                    print("WIFI_GUI: diagnostic dialog PASS", flush=True)
                    return False
            return True
        GLib.timeout_add(100, close_diagnostic)
        window.diagnostics()
        protected = {"ssid": b"fixture-secure", "security": 2, "signal": -40}
        row = window.store.append(["Protected fixture", "WPA2", "-40 dBm", protected])
        window.networks.get_selection().select_iter(row)
        def cancel_password():
            for dialog in Gtk.Window.list_toplevels():
                if dialog.get_title() == "输入 Wi-Fi 密码":
                    dialog.response(Gtk.ResponseType.CANCEL)
                    print("WIFI_GUI: password dialog cancel PASS", flush=True)
                    return False
            return True
        GLib.timeout_add(100, cancel_password)
        window.join_selected()
        assert connects == 0
        window.store.remove(row)
        window.networks.get_selection().select_path(0)
        window.buttons[1].emit("clicked")
        phase = 2
    elif phase == 2 and not window.busy and "已打开 Firefox" in window.status_label.get_text():
        assert connects == 1
        print("WIFI_GUI: open network connected without password; browser launched", flush=True)
        threading.Thread(target=browser_test, daemon=True).start()
        phase = 3
    elif phase == 3 and not window.busy and "联网检测通过" in window.status_label.get_text():
        assert desktop_owners() == original_owners, 'desktop component exited or restarted'
        print("WIFI_GUI: desktop processes unchanged", flush=True)
        print("WIFI_GUI: PASS open network + captive portal + browser login + online recheck", flush=True)
        phase = 4
    return True

GLib.timeout_add(250, advance)
context = GLib.MainContext.default()
while app.get_windows():
    context.iteration(True)
