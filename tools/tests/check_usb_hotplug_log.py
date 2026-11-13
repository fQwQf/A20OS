#!/usr/bin/env python3
"""Require live unplug/replug evidence; boot-report replay cannot satisfy it."""
import re
import sys
from pathlib import Path


def check(text):
    live = re.sub(r'^===== A20OS BOOT REPORT =====\n.*?^===== END BOOT REPORT =====\n?',
                  '', text, flags=re.MULTILINE | re.DOTALL)
    patterns = [r'\[USB-HID\] keyboard ready',
                r'\[USB\] port \d+ disconnected: address=1 reclaimed, slot=\d+ released',
                r'\[USB\] port \d+ slot=\d+ address=1',
                r'\[USB-HID\] keyboard ready']
    cursor = 0
    for pattern in patterns:
        match = re.search(pattern, live[cursor:])
        if not match:
            raise ValueError(f'missing live unplug/replug marker: {pattern}')
        cursor += match.end()


if __name__ == '__main__':
    try:
        check(Path(sys.argv[1]).read_text(errors='replace'))
    except ValueError as error:
        print(f'USB hotplug evidence: FAIL {error}', file=sys.stderr)
        sys.exit(1)
    print('USB hotplug evidence: PASS live unplug=1 replug=1 address_reused=1')
