#!/bin/sh
# Non-persistent SRAM load.  No erase, flash programming or bootloader update.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 <OpenOCD-interface.cfg>" >&2
    exit 2
fi
probe_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "${OPENOCD:-openocd}" -f "$1" -f "$probe_dir/openocd.cfg" \
    -c "init; halt; load_image {$probe_dir/probe.elf}; verify_image {$probe_dir/probe.elf}; resume 0x22010000; shutdown"
