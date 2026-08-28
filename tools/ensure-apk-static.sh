#!/bin/sh
# ensure-apk-static.sh — locate or fetch a static `apk` binary and print its
# absolute path on stdout.  Used by the A20OS package tooling (mka20repo.sh,
# mkrootfs.py) and safe to call repeatedly; downloads are cached under
# build/cache/apk-tools/.
#
# Env:
#   ALPINE_MIRROR_ROOT  mirror tree base (default: USTC, like the alpine
#                       rootfs builder)
#   APK_TOOLS_STATIC    exact package file name; when unset the latest
#                       apk-tools-static-*.apk is resolved (see below)
#   A20_CACHE_DIR       cache root override (default: build/cache)
#
# Version resolution order (only when APK_TOOLS_STATIC is unset):
#   1. HTML index of ALPINE_MIRROR_ROOT  (some mirrors truncate the listing!)
#   2. HTML index of the official CDN, then download from the mirror anyway
#   3. Download directly from the official CDN
#
# All diagnostics go to stderr so the script can be used as:
#   APK_STATIC=$(tools/ensure-apk-static.sh)

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

ALPINE_MIRROR_ROOT=${ALPINE_MIRROR_ROOT:-https://mirrors.ustc.edu.cn/alpine}
OFFICIAL_CDN=${A20_OFFICIAL_CDN:-https://dl-cdn.alpinelinux.org/alpine}
REPO_PATH=edge/main/x86_64
cache_root=${A20_CACHE_DIR:-$project_root/build/cache}
cache_dir="$cache_root/apk-tools/$(uname -m)"
extract_dir="$cache_dir/apk"

if [ -x "$extract_dir/sbin/apk.static" ]; then
    printf '%s\n' "$extract_dir/sbin/apk.static"
    exit 0
fi

mkdir -p "$cache_dir"

# resolve_latest LISTING_URL -> prints newest apk-tools-static file name
resolve_latest() {
    curl -fsSL --retry 3 --max-time 30 "$1" |
        grep -o 'apk-tools-static-[0-9][^"<]*\.apk' | sort -Vu | tail -n 1
}

download() {  # download BASE_URL FILE -> cache
    curl -fSL --retry 3 --max-time 120 -o "$pkg_path.tmp" "$1/$2" &&
        mv "$pkg_path.tmp" "$pkg_path"
}

if [ -n "${APK_TOOLS_STATIC:-}" ]; then
    pkg_name=$APK_TOOLS_STATIC
else
    pkg_name=$(resolve_latest "$ALPINE_MIRROR_ROOT/$REPO_PATH/" || true)
    if [ -z "$pkg_name" ]; then
        # Mirror HTML listings may be truncated (USTC caps at 1000 entries);
        # learn the version from the official CDN, still prefer the mirror
        # for the actual download.
        pkg_name=$(resolve_latest "$OFFICIAL_CDN/$REPO_PATH/" || true)
    fi
    if [ -z "$pkg_name" ]; then
        echo "ensure-apk-static: cannot resolve apk-tools-static version" >&2
        exit 1
    fi
fi

pkg_path="$cache_dir/$pkg_name"
if [ ! -f "$pkg_path" ]; then
    echo "ensure-apk-static: downloading $pkg_name" >&2
    download "$ALPINE_MIRROR_ROOT/$REPO_PATH" "$pkg_name" ||
        download "$OFFICIAL_CDN/$REPO_PATH" "$pkg_name" || {
            echo "ensure-apk-static: download failed for $pkg_name" >&2
            rm -f "$pkg_path.tmp"
            exit 1
        }
fi

rm -rf "$extract_dir.tmp"
mkdir -p "$extract_dir.tmp"
tar -xzf "$pkg_path" -C "$extract_dir.tmp"
rm -rf "$extract_dir"
mv "$extract_dir.tmp" "$extract_dir"

apk_static="$extract_dir/sbin/apk.static"
if [ ! -x "$apk_static" ]; then
    echo "ensure-apk-static: apk.static missing after extraction" >&2
    exit 1
fi
printf '%s\n' "$apk_static"
