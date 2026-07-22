#!/usr/bin/env bash
set -euo pipefail

# Preserve complete system cross sysroots (the normal Debian/Ubuntu setup).
# Fedora ships its RISC-V target packages in the secondary-architecture Koji
# repository instead, so extract only the runtime libraries into the project.
#
# Usage: prepare-riscv64-glibc-sysroot.sh \
#          <detected-system-lib-dir> <local-sysroot-root> [fedora-release]

SYSTEM_LIB_DIR="${1:-}"
LOCAL_ROOT="${2:?Usage: $0 <detected-system-lib-dir> <local-sysroot-root> [fedora-release]}"
REQUESTED_RELEASE="${3:-}"
LOCAL_LIB_DIR="$LOCAL_ROOT/lib"
REQUIRED_LIBS=(
    ld-linux-riscv64-lp64d.so.1
    libc.so.6
    libdl.so.2
    libm.so.6
    libpthread.so.0
    librt.so.1
    libatomic.so.1
    libgcc_s.so.1
)

runtime_complete() {
    local directory="$1" library

    [ -n "$directory" ] || return 1
    for library in "${REQUIRED_LIBS[@]}"; do
        [ -f "$directory/$library" ] || return 1
    done
}

if runtime_complete "$SYSTEM_LIB_DIR"; then
    echo "[GLIBC] Using system RISC-V runtime: $SYSTEM_LIB_DIR"
    exit 0
fi

if runtime_complete "$LOCAL_LIB_DIR"; then
    echo "[GLIBC] Using cached RISC-V runtime: $LOCAL_LIB_DIR"
    exit 0
fi

if [ -r /etc/os-release ]; then
    # This is distribution metadata, not project or user configuration.
    # shellcheck disable=SC1091
    . /etc/os-release
fi

case " ${ID:-} ${ID_LIKE:-} " in
    *fedora*) ;;
    *)
        echo "RISC-V glibc runtime not found in '$SYSTEM_LIB_DIR'." >&2
        echo "Automatic bootstrap is only used on Fedora; Debian/Ubuntu should provide /usr/riscv64-linux-gnu/lib." >&2
        echo "Set RISCV_GLIBC_LIB_DIR to a complete cross-runtime directory if it is installed elsewhere." >&2
        exit 1
        ;;
esac

for tool in dnf rpm2cpio cpio; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required Fedora bootstrap tool not found: $tool" >&2
        exit 1
    fi
done

FEDORA_RELEASE="${REQUESTED_RELEASE:-${VERSION_ID:-44}}"
REPOSITORY="https://riscv-koji.fedoraproject.org/repos-dist/f${FEDORA_RELEASE}/latest/riscv64/"
mkdir -p "$LOCAL_ROOT"
LOCAL_ROOT="$(cd "$LOCAL_ROOT" && pwd)"
LOCAL_LIB_DIR="$LOCAL_ROOT/lib"
WORK_DIR="$(mktemp -d "$LOCAL_ROOT/.bootstrap.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
mkdir -p "$WORK_DIR/rpms" "$WORK_DIR/root" "$LOCAL_LIB_DIR"

echo "[GLIBC] Downloading Fedora $FEDORA_RELEASE RISC-V runtime..."
for component in glibc libgcc libatomic; do
    downloaded=false
    for attempt in 1 2 3; do
        dnf --disablerepo='*' \
            --repofrompath=a20os-fedora-riscv,"$REPOSITORY" \
            download --destdir="$WORK_DIR/rpms" --arch=riscv64 \
            "$component" || true
        if compgen -G "$WORK_DIR/rpms/$component-*.riscv64.rpm" >/dev/null; then
            downloaded=true
            break
        fi
        echo "[GLIBC] Retrying $component download ($attempt/3)..." >&2
    done
    if [ "$downloaded" != true ]; then
        echo "Failed to download Fedora RISC-V package: $component" >&2
        exit 1
    fi
done

shopt -s nullglob
packages=("$WORK_DIR"/rpms/*.rpm)
if [ "${#packages[@]}" -ne 3 ]; then
    echo "Expected 3 Fedora RISC-V runtime packages, found ${#packages[@]}" >&2
    exit 1
fi

for package in "${packages[@]}"; do
    (
        cd "$WORK_DIR/root"
        rpm2cpio "$package" | cpio -idm --quiet
    )
done

for library in "${REQUIRED_LIBS[@]}"; do
    source_path="$(find "$WORK_DIR/root" \( -type f -o -type l \) -name "$library" -print -quit)"
    if [ -z "$source_path" ]; then
        echo "Downloaded Fedora runtime is missing $library" >&2
        exit 1
    fi
    cp -aL "$source_path" "$LOCAL_LIB_DIR/$library"
done

if ! runtime_complete "$LOCAL_LIB_DIR"; then
    echo "Failed to prepare the local RISC-V glibc runtime" >&2
    exit 1
fi

printf '%s\n' "$FEDORA_RELEASE" > "$LOCAL_ROOT/.fedora-release"
echo "[GLIBC] Prepared local RISC-V runtime: $LOCAL_LIB_DIR"
