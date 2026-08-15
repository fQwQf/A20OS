#!/bin/sh
# Build an Alpine Linux ext4 rootfs for A20OS, installing XFCE4 + the service
# layer (dbus / elogind / polkit / seatd / eudev) that the from-source GUI
# path stubs out.  The A20OS init chroots into this rootfs and hands control
# to /sbin/init when the a20-distro marker is present.
#
# Usage: user/rootfs/alpine/build.sh  (env: ARCH, OUTPUT, ROOTFS_SIZE_MB)

set -eu

: "${ARCH:?ARCH is required}"
: "${OUTPUT:?OUTPUT is required}"

ROOTFS_SIZE_MB=${ROOTFS_SIZE_MB:-8192}
# Mirror default: USTC is reliable and fast; switch with ALPINE_MIRROR_ROOT,
# e.g. https://mirrors.aliyun.com/alpine or the official CDN.
ALPINE_MIRROR_ROOT=${ALPINE_MIRROR_ROOT:-https://mirrors.ustc.edu.cn/alpine}
ALPINE_VERSION=${ALPINE_VERSION:-v3.23}
APK_TOOLS_STATIC=${APK_TOOLS_STATIC:-apk-tools-static-3.0.7-r0.apk}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
packages_file="$script_dir/packages.txt"
overlay_dir="$script_dir/overlay"
cache_dir="$project_root/build/cache/alpine/$(uname -m)"
output_dir=$(dirname -- "$OUTPUT")

case "$ARCH" in
  x86_64)    apk_arch=x86_64 ;;
  aarch64)   apk_arch=aarch64 ;;
  riscv64)   apk_arch=riscv64 ;;
  loongarch64) apk_arch=loongarch64 ;;
  *)
    echo "Alpine rootfs is unsupported for architecture: $ARCH" >&2
    exit 2 ;;
esac
package_cache_dir="$project_root/build/cache/alpine/packages/$apk_arch"

case "$ROOTFS_SIZE_MB" in
  ''|*[!0-9]*) echo "ROOTFS_SIZE_MB must be a positive integer" >&2; exit 2 ;;
esac
if [ "$ROOTFS_SIZE_MB" -le 1024 ]; then
  echo "ROOTFS_SIZE_MB is too small: $ROOTFS_SIZE_MB" >&2
  exit 2
fi

if [ "$(id -u)" -eq 0 ]; then sudo=; else sudo=${SUDO:-sudo}; fi

rootfs_dir="${TMPDIR:-/tmp}/a20os-alpine-rootfs.$$"
image_tmp="$OUTPUT.tmp.$$"
cleanup() { rm -rf "$rootfs_dir" "$image_tmp"; }
trap cleanup EXIT INT TERM

mkdir -p "$cache_dir" "$package_cache_dir" "$output_dir"

# --- Download apk-tools-static (from the same mirror tree) ---
apk_static_path="$cache_dir/$APK_TOOLS_STATIC"
apk_extract_dir="$cache_dir/apk"
apk_url="$ALPINE_MIRROR_ROOT/edge/main/x86_64/$APK_TOOLS_STATIC"
if [ ! -f "$apk_static_path" ]; then
  curl -L --fail -o "$apk_static_path.tmp" "$apk_url"
  mv "$apk_static_path.tmp" "$apk_static_path"
fi
if [ ! -d "$apk_extract_dir" ]; then
  rm -rf "$apk_extract_dir.tmp"
  mkdir -p "$apk_extract_dir.tmp"
  tar -xf "$apk_static_path" -C "$apk_extract_dir.tmp"
  mv "$apk_extract_dir.tmp" "$apk_extract_dir"
fi
apk_static="$apk_extract_dir/sbin/apk.static"
[ -x "$apk_static" ] || { echo "apk.static not found" >&2; exit 1; }

ALPINE_MIRROR="$ALPINE_MIRROR_ROOT/$ALPINE_VERSION"
$sudo mkdir -p "$rootfs_dir"

packages=$(sed -e 's/[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' "$packages_file")
# shellcheck disable=SC2086
$sudo "$apk_static" \
  --arch "$apk_arch" \
  -U --allow-untrusted \
  --root "$rootfs_dir" \
  --cache-dir "$package_cache_dir" \
  --cache-packages \
  -X "$ALPINE_MIRROR/main" \
  -X "$ALPINE_MIRROR/community" \
  --initdb add $packages

# --- Apply overlay ---
$sudo cp -a "$overlay_dir/." "$rootfs_dir/"

printf '%s\n' "$ALPINE_MIRROR/main" "$ALPINE_MIRROR/community" | \
  $sudo tee "$rootfs_dir/etc/apk/repositories" >/dev/null

$sudo chmod 0755 \
  "$rootfs_dir/usr/lib/a20/init" \
  "$rootfs_dir/usr/lib/a20/start-xfce4-session"
$sudo chmod 0440 "$rootfs_dir/etc/sudoers" 2>/dev/null || true
$sudo chmod 0400 "$rootfs_dir/etc/shadow" 2>/dev/null || true
$sudo ln -snf /usr/lib/a20/init "$rootfs_dir/sbin/init"
$sudo ln -snf /usr/share/zoneinfo/Asia/Shanghai "$rootfs_dir/etc/localtime"

# --- Generate caches apk.static skips (post-install scripts don't run) ---
if [ -x "$rootfs_dir/usr/bin/gdk-pixbuf-query-loaders" ]; then
  $sudo chroot "$rootfs_dir" /usr/bin/gdk-pixbuf-query-loaders --update-cache || true
fi
if [ -x "$rootfs_dir/usr/bin/update-mime-database" ]; then
  $sudo chroot "$rootfs_dir" /usr/bin/update-mime-database /usr/share/mime || true
fi

# --- Build ext4 image ---
truncate -s "${ROOTFS_SIZE_MB}M" "$image_tmp"
$sudo mkfs.ext4 -q -F -b 1024 -I 256 \
  -O extent,64bit,flex_bg,huge_file,dir_nlink,extra_isize,dir_index,metadata_csum,^has_journal,^quota,^metadata_csum_seed,^orphan_file,^project,^encrypt,^verity,^casefold,^inline_data,^ea_inode,^bigalloc,^mmp,^fast_commit,^sparse_super2 \
  -E lazy_itable_init=0 \
  -L a20os-rootfs -d "$rootfs_dir" "$image_tmp"
chmod 0644 "$image_tmp"
mv -f "$image_tmp" "$OUTPUT"

trap - EXIT INT TERM
cleanup
