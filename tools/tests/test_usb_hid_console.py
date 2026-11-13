"""Host-side checks for the USB HID boot-keyboard console bridge."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'kernel/drivers/usb/class/usb_hid.c'


class UsbHidConsoleTest(unittest.TestCase):
    def test_ascii_translation_and_console_sink(self):
        source = SOURCE.read_text()
        start = source.index('static char usb_hid_usage_ascii')
        end = source.index('static int usb_hid_console_key')
        body = source[start:end]
        self.assertIn('uart_receive_char(c)', source)
        self.assertIn('if (!fbcon_active())', source)
        harness = r'''
#include <assert.h>
#include <stdint.h>
''' + body + r'''
int main(void) {
    assert(usb_hid_usage_ascii(0x04, 0) == 'a');
    assert(usb_hid_usage_ascii(0x04, 1) == 'A');
    assert(usb_hid_usage_ascii(0x1e, 0) == '1');
    assert(usb_hid_usage_ascii(0x1e, 1) == '!');
    assert(usb_hid_usage_ascii(0x28, 0) == '\n');
    assert(usb_hid_usage_ascii(0x2a, 0) == 0x7f);
    assert(usb_hid_usage_ascii(0x2b, 0) == '\t');
    assert(usb_hid_usage_ascii(0x2c, 0) == ' ');
    assert(usb_hid_usage_ascii(0x2f, 1) == '{');
    assert(usb_hid_usage_ascii(0x38, 1) == '?');
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-usbhid-') as tmp:
            path = Path(tmp) / 'test.c'
            path.write_text(harness)
            exe = Path(tmp) / 'test'
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
