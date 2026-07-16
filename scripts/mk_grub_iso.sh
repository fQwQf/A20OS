#!/bin/bash
# Create a bootable GRUB2 rescue ISO for A20OS x86_64 Multiboot kernel.
# Usage: mk_grub_iso.sh <kernel.elf> <output.iso> [grub-cfg-args]

set -e

KERNEL_ELF="${1:-}"
OUTPUT_ISO="${2:-}"

if [ -z "$KERNEL_ELF" ] || [ -z "$OUTPUT_ISO" ]; then
    echo "Usage: $0 <kernel.elf> <output.iso>"
    exit 1
fi

if [ ! -f "$KERNEL_ELF" ]; then
    echo "Error: kernel ELF not found: $KERNEL_ELF"
    exit 1
fi

GRUB_MKRESCUE=""
for cmd in grub-mkrescue grub2-mkrescue; do
    if command -v "$cmd" >/dev/null 2>&1; then
        GRUB_MKRESCUE="$cmd"
        break
    fi
done

if [ -z "$GRUB_MKRESCUE" ]; then
    echo "Error: grub-mkrescue not found. Install GRUB2 (e.g. apt install grub-pc-bin grub-common)."
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

mkdir -p "$TMPDIR/boot/grub"

cp "$KERNEL_ELF" "$TMPDIR/boot/kernel.elf"

cat > "$TMPDIR/boot/grub/grub.cfg" <<'EOF'
set timeout=0
set default=0

menuentry "A20OS" {
    multiboot /boot/kernel.elf
    boot
}
EOF

echo "Building GRUB ISO with $GRUB_MKRESCUE..."
"$GRUB_MKRESCUE" -o "$OUTPUT_ISO" "$TMPDIR" \
    --modules="multiboot normal boot rescue serial terminal linux configfile" \
    2>/dev/null || "$GRUB_MKRESCUE" -o "$OUTPUT_ISO" "$TMPDIR"

echo "ISO created: $OUTPUT_ISO"
