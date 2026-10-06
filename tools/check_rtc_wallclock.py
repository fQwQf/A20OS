#!/usr/bin/env python3
"""Check the wall clock a guest reported against the host clock.

This is the host half of smoke-rtc-cmos.  The in-guest assertion cannot be
made from the guest log alone: the whole point of the gate is whether the
kernel's wall clock agrees with real time, and only the host knows what real
time is.  The guest is asked to print

    /bin/date -u +RTC_WALLCLOCK=%Y-%m-%dT%H:%M:%SZ

and this script re-asserts, on the host:

  * the marker is present at all (a missing marker fails, it is not skipped);
  * the reported time is at least --min-epoch, so a clock stuck at 1970 or a
    guest that answered with something else entirely cannot pass;
  * |guest - host| <= --max-skew, so a guest still running on the kernel's
    build-time seed also passes.  That is deliberate: this check proves the
    guest can read a plausible clock, and smoke-rtc-cmos's `expect` list is
    what proves *where* that clock came from (the CMOS RTC line).  The build
    timestamp and the CMOS RTC are close to each other by construction, so no
    skew bound can tell them apart -- only the driver's own log line can.
"""

from __future__ import annotations

import calendar
import re
import sys
import time

MARKER = re.compile(r"RTC_WALLCLOCK=(\d{4})-(\d{2})-(\d{2})T"
                    r"(\d{2}):(\d{2}):(\d{2})Z")


def main() -> int:
    max_skew = 300
    min_epoch = calendar.timegm((2026, 1, 1, 0, 0, 0, 0, 0, 0))
    args = sys.argv[1:]
    while len(args) >= 2 and args[0] in ("--max-skew", "--min-epoch"):
        option = args.pop(0)
        try:
            value = int(args.pop(0))
        except ValueError:
            value = -1
        if value < 0:
            print(f"check_rtc_wallclock: invalid value for {option}",
                  file=sys.stderr)
            return 2
        if option == "--max-skew":
            max_skew = value
        else:
            min_epoch = value
    if len(args) != 1:
        print(f"usage: {sys.argv[0]} [--max-skew S] [--min-epoch S] LOG",
              file=sys.stderr)
        return 2
    path = args[0]

    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        print(f"check_rtc_wallclock: cannot read {path}: {error}",
              file=sys.stderr)
        return 1

    samples = []
    for match in MARKER.finditer(text):
        parts = [int(part) for part in match.groups()]
        try:
            stamp = calendar.timegm(tuple(parts) + (0, 0, 0))
        except (ValueError, OverflowError):
            continue  # not a real calendar time; do not let it pass silently
        samples.append((match.group(0), stamp))

    if not samples:
        print(f"check_rtc_wallclock: no RTC_WALLCLOCK= line in {path}; the "
              f"guest never reported a wall clock", file=sys.stderr)
        return 1

    now = int(time.time())
    failed = False
    for marker, stamp in samples:
        skew = stamp - now
        if stamp < min_epoch:
            print(f"check_rtc_wallclock: FAIL {marker} -> epoch {stamp} is "
                  f"before the required minimum {min_epoch} "
                  f"(2026-01-01T00:00:00Z)", file=sys.stderr)
            failed = True
            continue
        if abs(skew) > max_skew:
            print(f"check_rtc_wallclock: FAIL {marker} -> epoch {stamp} is "
                  f"{skew:+d}s from the host clock {now}, outside "
                  f"+/-{max_skew}s", file=sys.stderr)
            failed = True
            continue
        print(f"check_rtc_wallclock: {marker} -> epoch {stamp} "
              f"({skew:+d}s from host)")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())