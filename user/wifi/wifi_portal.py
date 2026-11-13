# SPDX-License-Identifier: BSD-3-Clause
"""Captive portal checks. Account entry belongs to the normal browser."""
from pathlib import Path
import json
import ssl
import subprocess
import urllib.error
import urllib.parse
import urllib.request

HTTP_PROBE = "http://connect.rom.miui.com/generate_204"
HTTPS_PROBE = "https://connect.rom.miui.com/generate_204"
MANUAL_LOGIN = "http://neverssl.com/"


def safe_url(value, https_only=False):
    if not isinstance(value, str) or len(value) > 2048 or any(ord(c) <= 32 or ord(c) == 127 for c in value):
        return None
    try:
        url = urllib.parse.urlsplit(value)
        if url.scheme not in (("https",) if https_only else ("http", "https")):
            return None
        if not url.hostname or url.username is not None or url.password is not None:
            return None
        _ = url.port
        return value
    except ValueError:
        return None


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, newurl):
        return None


def fetch(url, accept="*/*"):
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), NoRedirect(),
        urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache",
                                                  "Accept": accept,
                                                  "User-Agent": "A20OS-Connectivity/1"})
    try:
        response = opener.open(request, timeout=8)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        body = response.read(65537)
        if len(body) > 65536:
            body = body[:65536]
        return response.status, response.headers, body


def capport_uri(path="/run/a20-wifi-capport"):
    try:
        value = Path(path).read_text().strip()
        if not value.startswith("https://"):
            value = bytes.fromhex(value).decode("utf-8")
        return safe_url(value, https_only=True)
    except (OSError, ValueError, UnicodeError):
        return None


def detect(http_url=HTTP_PROBE, https_url=HTTPS_PROBE, api=None, request=fetch):
    """A DHCP lease alone never establishes internet reachability."""
    if api and safe_url(api, https_only=True):
        try:
            code, headers, body = request(api, "application/captive+json")
            state = json.loads(body) if code == 200 else {}
            if not isinstance(state, dict):
                state = {}
            login = safe_url(state.get("user-portal-url"), https_only=True)
            if state.get("captive") is True:
                return {"state": "portal", "login_url": login or MANUAL_LOGIN,
                        "message": "已连接 Wi-Fi，需要在网页中认证"}
        except (OSError, ValueError):
            pass
    try:
        code, headers, body = request(http_url)
        if 300 <= code < 400:
            location = headers.get("Location", "")
            login = safe_url(urllib.parse.urljoin(http_url, location)) if location else None
            return {"state": "portal", "login_url": login or MANUAL_LOGIN,
                    "message": "已连接 Wi-Fi，需要在网页中认证"}
        if code in (200, 511) or (code == 204 and body):
            return {"state": "portal", "login_url": http_url,
                    "message": "网络返回了认证页面，请在浏览器中登录"}
        if code != 204:
            raise ValueError("unexpected connectivity response")
        code, headers, body = request(https_url)
        if code == 204 and not body:
            return {"state": "online", "login_url": MANUAL_LOGIN,
                    "message": "联网检测通过（HTTP 和 HTTPS）"}
    except (OSError, ValueError):
        pass
    if http_url == HTTP_PROBE and https_url == HTTPS_PROBE:
        return detect("http://connectivitycheck.gstatic.com/generate_204",
                      "https://connectivitycheck.gstatic.com/generate_204",
                      request=request)
    return {"state": "unknown", "login_url": MANUAL_LOGIN,
            "message": "已连接 Wi-Fi，尚未确认外网可用；可手动打开认证页"}


def open_login(url):
    if not safe_url(url):
        raise ValueError("认证地址必须为 HTTP 或 HTTPS 网页")
    # The short-lived shell completes its exec handshake before Firefox's
    # multi-process startup. The URL is an argument, never interpolated code.
    return subprocess.Popen(["/bin/sh", "-c", 'exec firefox --new-tab "$1"',
                             "a20-wifi-login", url], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
