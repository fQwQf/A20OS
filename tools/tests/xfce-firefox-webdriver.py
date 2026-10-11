#!/usr/bin/env python3
"""Guest-only Firefox network/DOM regression through localhost Marionette.

Launch Firefox with --marionette in a disposable XFCE test image first. This
fixture is not part of the production overlay. Protocol reference:
https://firefox-source-docs.mozilla.org/remote/marionette/Protocol.html
"""
import json
import socket
import time


class Firefox:
    def __init__(self):
        deadline = time.monotonic() + 180
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', 2828), timeout=10)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(2)
        self.sock.settimeout(150)
        self.stream = self.sock.makefile('rb')
        self.sequence = 0
        hello = self.receive()
        assert hello.get('marionetteProtocol') == 3, hello

    def receive(self):
        prefix = bytearray()
        while True:
            byte = self.stream.read(1)
            if not byte:
                raise EOFError('Firefox closed Marionette connection')
            if byte == b':':
                break
            prefix.extend(byte)
            if len(prefix) > 12 or not byte.isdigit():
                raise ValueError('invalid Marionette frame prefix')
        size = int(prefix)
        data = self.stream.read(size)
        if len(data) != size:
            raise EOFError('short Marionette frame')
        return json.loads(data)

    def command(self, name, **parameters):
        self.sequence += 1
        request = json.dumps([0, self.sequence, name, parameters]).encode()
        self.sock.sendall(str(len(request)).encode() + b':' + request)
        response = self.receive()
        assert response[:2] == [1, self.sequence], response
        if response[2]:
            raise RuntimeError(f'{name}: {response[2]}')
        return response[3]

    def script(self, text):
        return self.command('WebDriver:ExecuteScript', script=text, args=[],
                            newSandbox=True, sandbox='default', line=1,
                            filename='xfce-firefox-webdriver.py')['value']

    def wait_script(self, text, timeout=120):
        deadline = time.monotonic() + timeout
        while True:
            value = self.script(text)
            if value:
                return value
            if time.monotonic() >= deadline:
                raise TimeoutError('page DOM did not satisfy: ' + text)
            time.sleep(2)


def main():
    browser = Firefox()
    session = browser.command('WebDriver:NewSession',
                              acceptInsecureCerts=False, pageLoadStrategy='eager')
    assert session['capabilities']['acceptInsecureCerts'] is False
    assert session['capabilities']['pageLoadStrategy'] == 'eager'
    print('XFCE_WEB: session', json.dumps(session), flush=True)
    browser.command('WebDriver:SetTimeouts', pageLoad=120000, script=30000)
    for scheme in ('http', 'https'):
        browser.command('WebDriver:Navigate', url=f'{scheme}://example.org/')
        browser.wait_script("return /Example Domain/i.test(document.title) && document.body && document.body.innerText.length > 100;")
        assert browser.script('return location.protocol;') == scheme + ':'
        print(f'XFCE_WEB: Firefox {scheme.upper()} DOM PASS', flush=True)
    browser.command('WebDriver:Navigate', url='https://www.bing.com/')
    browser.wait_script("return !!document.querySelector('input[name=q],textarea[name=q]');")
    assert browser.script('return location.hostname;').endswith('bing.com')
    print('XFCE_WEB: Firefox Bing home DOM PASS', flush=True)
    field = browser.command('WebDriver:FindElement', using='css selector',
                            value='input[name=q],textarea[name=q]')['value']
    element = field['element-6066-11e4-a52e-4f735466cecf']
    browser.command('WebDriver:ElementClick', id=element)
    browser.command('WebDriver:ElementSendKeys', id=element, text='linux\ue007')
    results = browser.wait_script("""return document.querySelectorAll('#b_results .b_algo h2 a').length;""")
    title = browser.command('WebDriver:GetTitle')['value']
    assert 'linux' in title.lower(), title
    print(f'XFCE_WEB: Firefox Bing results PASS count={results} title={title!r}', flush=True)
    browser.command('WebDriver:Refresh')
    browser.wait_script("return document.querySelectorAll('#b_results .b_algo h2 a').length;")
    print('XFCE_WEB: Firefox refresh PASS', flush=True)
    original = browser.command('WebDriver:GetWindowHandle')['value']
    tab = browser.command('WebDriver:NewWindow', type='tab')
    browser.command('WebDriver:SwitchToWindow', handle=tab['handle'])
    browser.command('WebDriver:Navigate', url='https://example.org/')
    browser.wait_script("return /Example Domain/i.test(document.title) && document.body && document.body.innerText.length > 100;")
    browser.command('WebDriver:CloseWindow')
    browser.command('WebDriver:SwitchToWindow', handle=original)
    # Continue executing in the same content process across a five-minute dwell.
    for minute in range(5):
        time.sleep(60)
        browser.wait_script("return document.querySelectorAll('#b_results .b_algo h2 a').length;", timeout=15)
        print(f'XFCE_WEB: Firefox live DOM minute={minute + 1} PASS', flush=True)
    browser.command('WebDriver:DeleteSession')
    print('XFCE_WEB: Firefox PASS', flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'XFCE_WEB: Firefox FAIL {error!r}', flush=True)
        raise
