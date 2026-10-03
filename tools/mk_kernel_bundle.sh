#!/bin/sh
# Assemble a boot handoff bundle for a board that has no bootable storage.
#
# The SBC ports that have no block driver cannot be given an SD card: there is
# nothing to put a filesystem on.  What they can be given is the kernel itself,
# plus the exact commands that hand it to the boot chain already running on the
# board.  This writes those into one directory so the operator has a single
# thing to transfer, and so the commands in the repository and the commands
# typed on the board are the same commands.
#
# The device tree is deliberately NOT shipped here.  Every board in this family
# boots through a vendor FSBL and U-Boot, and U-Boot passes its own board DTB in
# a1 -- that DTB is the authoritative description of the board, and shipping a
# second copy from the kernel tree would be a second thing to keep in sync with
# the firmware.  The handoff script loads the kernel only and lets U-Boot supply
# the DTB, which is what the in-tree ports assume when riscv64_memory_init()
# reads __boot_dtb_ptr.
#
# usage: mk_kernel_bundle.sh <kernel.elf> <kernel.bin> <out-dir> <board>
set -eu

if [ "$#" -ne 4 ]; then
	echo "usage: $0 <kernel.elf> <kernel.bin> <out-dir> <board>" >&2
	exit 2
fi

elf=$1
kernel=$2
outdir=$3
board=$4

if [ ! -f "$kernel" ]; then
	echo "$0: no kernel at $kernel" >&2
	exit 1
fi

# The load address is whatever the board's linker script linked the image at,
# read back out of the first PT_LOAD header rather than repeated per board: the
# QEMU virt default is 0x80200000 and a board ldscript moves it, so a table here
# would be a second place to forget.
load_addr=$(READELF="${READELF:-readelf}" readelf -lW "$elf" 2>/dev/null \
	| awk '$1 == "LOAD" { print $4; exit }')
if [ -z "$load_addr" ]; then
	echo "$0: no PT_LOAD segment in $elf" >&2
	exit 1
fi

mkdir -p "$outdir"
cp "$kernel" "$outdir/kernel.bin"

# U-Boot's `booti` takes the kernel, the device tree it already loaded, and an
# initramfs pointer.  Passing the DTB it loaded itself is what makes the kernel's
# __boot_dtb_ptr valid; the third argument is 0 because there is no initrd here.
cat >"$outdir/uboot.cmd" <<EOF
# Handoff for $board.
#
# Interrupt U-Boot's autoboot, then run these lines.  \$filesize is set by the
# load step and \$loadaddr is where the kernel landed; booti reads both.
loadaddr=$load_addr
filesize=\${filesize}
booti \$loadaddr - \$loadaddr
EOF

cat >"$outdir/README.md" <<EOF
# $board handoff bundle

This board has no block driver yet, so there is no SD card to write.  Boot the
vendor FSBL and U-Boot as the board's own documentation says, interrupt
autoboot, then transfer \`kernel.bin\` and run \`uboot.cmd\`.

No device tree is included: U-Boot passes its own board DTB in a1, which is what
the kernel reads for RAM, console and the platform device tree.  If the kernel
prints \`[FDT] memory node unavailable\` instead of a RAM range, U-Boot did not
hand the DTB over and the load is wrong, not the kernel.

Evidence that this worked, not just that it booted: the log must contain a
\`[FDT] RAM range\` line and \`System ready\`.  Anything less is a partial boot.
EOF

echo "handoff bundle for $board: $outdir (load at $load_addr)"
ls -l "$outdir"
