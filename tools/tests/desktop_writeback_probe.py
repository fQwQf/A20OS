#!/usr/bin/env python3
"""Guest sync/fsync regression. Run `write`, then `verify` after a real reboot.

A warm-cache verification alone cannot establish durability. Alternatively,
verify debugfs dumps from the stopped guest image against the printed digest.
"""
import argparse
import hashlib
import os
from pathlib import Path
import threading
import time


def page(index):
    return hashlib.sha256(str(index).encode()).digest() * 128


def verify(root, rounds, pages):
    expected = hashlib.sha256()
    for index in range(pages):
        expected.update(page(index))
    for run in range(rounds):
        path = root / ("writeback-%d.bin" % run)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected.hexdigest():
            raise RuntimeError("%s: %s != %s" % (path, actual, expected.hexdigest()))
        print("VERIFY", path, actual, flush=True)


def write(root, rounds, pages):
    root.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()
    failures = []

    def syncer():
        try:
            while not stop.is_set():
                os.sync()
                time.sleep(.001)
        except BaseException as error:
            failures.append(error)

    thread = threading.Thread(target=syncer)
    thread.start()
    try:
        for run in range(rounds):
            path = root / ("writeback-%d.bin" % run)
            fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
            try:
                for index in range(pages):
                    if os.pwrite(fd, page(index), index * 4096) != 4096:
                        raise RuntimeError("short pwrite")
                    if index % 31 == 0:
                        os.fsync(fd)
                os.fsync(fd)
            finally:
                os.close(fd)
            print("WROTE", path, pages * 4096, flush=True)
    finally:
        stop.set()
        thread.join()
    if failures:
        raise failures[0]
    os.sync()
    verify(root, rounds, pages)
    print("Warm-cache check passed; repeat verify after reboot.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("write", "verify"))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--pages", type=int, default=2048)
    args = parser.parse_args()
    if args.rounds < 1 or args.pages < 1:
        parser.error("rounds and pages must be positive")
    (write if args.mode == "write" else verify)(args.directory, args.rounds, args.pages)


if __name__ == "__main__":
    main()
