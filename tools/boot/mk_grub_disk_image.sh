#!/bin/sh
# Build a directly bootable x86_64 disk image for VirtualBox / QEMU / bare metal.
#
# The existing x86_64 VirtualBox artifact is a GRUB rescue ISO, which means optical
# media: it boots, but it is not a disk, and a plain VM wants a disk.  This produces
# the same shape the aarch64 VirtualBox image already has -- a raw GPT disk with a
# single FAT32 EFI System Partition -- so both architectures hand an operator the
# same kind of thing, and both boot with no ISO in the tray and no second device.
#
# This target packages the Multiboot kernel behind GRUB on a GPT/ESP disk.
# Architecture-owned standalone EFI loaders are separate build targets.
# See docs/platforms/x86_64-pc.md for deployment requirements.
#
# GRUB is built with grub-mkimage against an explicit /boot/grub prefix, and the
# modules live on the ESP.  A grub-mkstandalone image was tried first and did boot,
# but never resolved a root device: `search --file /boot/kernel.elf` returned "no
# such device" and multiboot could not find a file sitting on the very ESP that had
# just loaded it, because the standalone memdisk prefix does not line up with where
# the modules it wanted live.  A real prefix on the ESP is the layout grub-install
# produces, and the documented one.
#
# usage: mk_grub_disk_image.sh <kernel.elf> <output.img> [rootfs-fat32.img]
set -eu

kernel_elf=${1:-}
output=${2:-}
rootfs_image=${3:-}

if [ -z "$kernel_elf" ] || [ -z "$output" ]; then
	echo "Usage: $0 <kernel.elf> <output.img> [rootfs-fat32.img]" >&2
	exit 2
fi
if [ ! -f "$kernel_elf" ]; then
	echo "Error: kernel missing: $kernel_elf" >&2
	exit 1
fi

grub_dir=${GRUB_EFI_DIR:-/usr/lib/grub/x86_64-efi}
for tool in parted mformat mcopy grub-mkimage sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "Error: $tool not found (install parted, mtools, grub-efi-amd64-bin)" >&2
		exit 1
	}
done
[ -d "$grub_dir" ] || {
	echo "Error: no GRUB module directory at $grub_dir (install grub-efi-amd64-bin)" >&2
	exit 1
}

log_mb=${A20_X86_LOG_MB:-64}
log_session=${A20_LOG_SESSION:-1}
base_mb=${A20_X86_DISK_MB:-512}
if [ "$log_mb" -gt 0 ]; then
	size_mb=$((base_mb + log_mb + 1))
else
	size_mb=$base_mb
fi
esp_mb=$((base_mb - 1))
log_start_mb=$((esp_mb + 1))
log_end_mb=$((log_start_mb + log_mb))

mkdir -p "$(dirname "$output")"
rm -f "$output"
truncate -s "${size_mb}M" "$output"

# One ESP holding the EFI loader, GRUB, the kernel and -- when given -- the FAT32
# root.  The aarch64 image puts its stub and its root on the same ESP for the same
# reason: a single writable volume is one thing to attach and one thing to get wrong.
parted -s "$output" mklabel gpt
parted -s "$output" mkpart ESP fat32 1MiB "${esp_mb}MiB"
parted -s "$output" set 1 esp on
if [ "$log_mb" -gt 0 ]; then
	parted -s "$output" mkpart log fat32 "${log_start_mb}MiB" "${log_end_mb}MiB"
	parted -s "$output" name 2 '"A20OS log"'
fi

esp_offset=$((1024 * 1024))
mformat -i "$output@@$esp_offset" -F -v A20OS ::
if [ "$log_mb" -gt 0 ]; then
	log_offset=$((log_start_mb * 1024 * 1024))
	log_sectors=$((log_mb * 1024 * 1024 / 512))
	mformat -i "$output@@$log_offset" -T "$log_sectors" -F -v A20OSLOG ::
fi

# mktemp honours TMPDIR; pin it into the checkout (make exports build/tmp for
# recipes, this covers a manual run) so the scratch files land under build/,
# where `make clean` can remove them.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMPDIR="${TMPDIR:-$REPO_ROOT/build/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"

mtoolsrc=$(mktemp)
tmp_cfg=$(mktemp)
tmp_efi=$(mktemp)
tmp_readme=$(mktemp)
trap 'rm -f "$mtoolsrc" "$tmp_cfg" "$tmp_efi" "$tmp_readme"' EXIT HUP INT TERM

# mtools maps one image per drive, and the rootfs is itself a FAT32 image file, so
# it becomes a second drive rather than a glob -- the way the aarch64 image does it.
{
	echo "drive b: file=\"$output\" offset=$esp_offset"
	[ -n "$rootfs_image" ] && echo "drive a: file=\"$rootfs_image\""
} >"$mtoolsrc"

mcopy_() { MTOOLSRC="$mtoolsrc" mcopy "$@"; }
mmd_() { MTOOLSRC="$mtoolsrc" mmd "$@"; }

for d in b:/EFI b:/EFI/BOOT b:/boot b:/boot/grub b:/boot/grub/x86_64-efi; do
	mmd_ "$d"
done

# The search is load-bearing, not decoration: it picks the volume by file rather than
# by firmware device numbering, which differs between OVMF, VirtualBox and hardware.
cat >"$tmp_cfg" <<'EOF'
search --no-floppy --set=root --file /boot/kernel.elf
multiboot /boot/kernel.elf
boot
EOF
mcopy_ "$tmp_cfg" b:/boot/grub/grub.cfg

# grub-mkimage takes modules positionally; --install-modules is a
# grub-mkstandalone option and is rejected here.  `normal` is deliberately absent:
# it loads every filesystem module it can see, which on a machine with one FAT
# partition is pure noise.  The modules actually needed are named instead.
grub-mkimage -O x86_64-efi -p /boot/grub -o "$tmp_efi" \
	part_gpt fat ext2 multiboot search search_fs_uuid search_fs_file \
	linux configfile echo test minicmd ls cat halt reboot


# Ship the whole module directory rather than the subset grub-mkimage resolved.  The
# ESP has hundreds of megabytes free and a missing .mod shows up as a boot that
# stops at a grub> prompt, which is a much worse failure than a slightly larger image.
for mod in "$grub_dir"/*.mod; do
	mcopy_ -o "$mod" b:/boot/grub/x86_64-efi/
done

mcopy_ -o "$tmp_efi" b:/EFI/BOOT/BOOTX64.EFI
mcopy_ "$kernel_elf" b:/boot/kernel.elf

if [ -n "$rootfs_image" ]; then
	if [ ! -f "$rootfs_image" ]; then
		echo "Error: rootfs image missing: $rootfs_image" >&2
		exit 1
	fi
	# /boot already exists, so the root merges in beside it rather than replacing it.
	mcopy_ -s 'a:/*' b:/
fi

bootx64_sha=$(sha256sum "$tmp_efi" | awk '{print $1}')
kernel_sha=$(sha256sum "$kernel_elf" | awk '{print $1}')

printf 'format=a20os-x86_64-uefi-gpt-v1\n'
printf 'esp_offset=%s\n' "$esp_offset"
printf 'boots=%s\n' 'uefi:EFI/BOOT/BOOTX64.EFI'
printf 'grub_prefix=%s\n' '/boot/grub'
printf 'bootx64_sha256=%s\n' "$bootx64_sha"
printf 'kernel_sha256=%s\n' "$kernel_sha"
if [ "$log_mb" -gt 0 ]; then
	cat >"$tmp_readme" <<'EOF'
A20OS persistent log partition

The kernel writes boot-<id>.jsonl here. See docs/zh/kernel/persistent-logging.md.
EOF
	mcopy -o -i "$output@@$log_offset" "$tmp_readme" ::/README.txt
	printf 'schema=1\nsession=%s\n' "$log_session" >"$tmp_readme"
	mcopy -o -i "$output@@$log_offset" "$tmp_readme" ::/CONFIG
	printf 'log_partition_name=A20OS log\n'
	printf 'log_offset_bytes=%s\n' "$log_offset"
	printf 'log_size_mb=%s\n' "$log_mb"
	printf 'log_session=%s\n' "$log_session"
fi
echo "uefi disk image ready: $output"
