#!/usr/bin/env bash
# Build a host virglrenderer new enough for a current Mesa.
#
# Why this exists
# ---------------
# GPU_3D=1 makes A20OS attach a virgl-capable virtio-gpu, but the *renderer*
# lives on the host: QEMU dlopen()s libvirglrenderer and hands it the guest's
# GL command stream.  So 3D availability is gated by the host's virglrenderer,
# not by the guest kernel.  A virglrenderer too old to build the offscreen
# desktop-GL context that virgl_renderer_get_capset() needs makes Mesa's
# virtio_gpu_dri.so attach fail with ERR_INVALID_PARAMETER, and no change to
# A20OS can fix that.
#
# Distributions still ship 1.1.0 (2020) on some releases -- Debian trixie has
# no newer candidate -- so the only route is to build it.  That needs root, so
# it is a script an operator runs deliberately rather than something the build
# does behind their back.
#
# Usage
#   tools/build-virglrenderer.sh check     report host state and missing deps
#   tools/build-virglrenderer.sh build     fetch, build, install under tools/virgl
#   tools/build-virglrenderer.sh env       print the LD_LIBRARY_PATH to use
#
# After `build`, point the guest launcher at the result (the Makefile picks up
# the conventional prefix automatically when it exists):
#   make ARCH=riscv64 GPU_3D=1 QEMU_MEMORY=2G run-world-gui
# Override with VIRGL_PREFIX=/some/where if you built elsewhere.

set -euo pipefail

# Upstream is gitlab.freedesktop.org/virgl/virglrenderer -- NOT under mesa/,
# which is a different (nonexistent for this project) path and answers with a
# sign-in page rather than a 404, so a wrong path here looks like an auth
# problem instead of a typo.  The archive is .tar.bz2, and releases are tagged
# both as "1.3.0" and "virglrenderer-1.3.0"; the latter is the release tag.
VIRGL_VERSION="${VIRGL_VERSION:-1.3.0}"
VIRGL_URL="${VIRGL_URL:-https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/virglrenderer-${VIRGL_VERSION}/virglrenderer-virglrenderer-${VIRGL_VERSION}.tar.bz2}"
VIRGL_REPO="https://gitlab.freedesktop.org/mesa/virglrenderer"

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${VIRGL_PREFIX:-${REPO_ROOT}/tools/virgl/install}"
SRC_DIR="${REPO_ROOT}/tools/virgl/src"
BUILD_DIR="${REPO_ROOT}/tools/virgl/build"

# Beyond 0.8.2 QEMU can pass GL through at all; 1.0.0 adds the Venus protocol a
# modern Mesa prefers.  Anything below the first is useless to us.  1.3.0 is the
# newest release and the one this script is verified to build.
MIN_VIRGL=0.8.2
# A 2020-era 1.1.0 reports a capset of ~308 bytes.  A renderer with the modern
# protocol reports kilobytes, because the capset has to describe shader
# capabilities, format tables and parameter limits.  The size is therefore a
# usable age check, and it is what the guest log prints.
STALE_CAPSET_BYTES=1024

log()  { printf '%s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# pkg-config names differ from apt package names.
# python3-yaml is not optional: 1.3.0's gallium build imports it, and meson
# reports the missing module only after a long configure, so a "check" that
# omits it passes and then the build dies two minutes in.
APT_PACKAGES=(
    meson ninja-build pkg-config flex bison
    libepoxy-dev libgbm-dev libdrm-dev libudev-dev
    python3-mako python3-yaml libgl-dev libegl-dev
)
# llvm-dev is only needed for the gallium/virgl shader path; name it separately
# so its absence is reported as optional rather than as a hard dependency.
OPTIONAL_PACKAGES=(llvm-dev libva-dev libvulkan-dev)

pkg_for_provides() {
    case "$1" in
        meson) echo python3 ;;
        *) echo "$1" ;;
    esac
}

report_deps() {
    local missing=() optional_missing=() p pkg
    for p in "${APT_PACKAGES[@]}"; do
        pkg="$(pkg_for_provides "$p")"
        if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
            missing+=("$pkg")
        fi
    done
    for p in "${OPTIONAL_PACKAGES[@]}"; do
        pkg="$(pkg_for_provides "$p")"
        if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "ok installed"; then
            optional_missing+=("$pkg")
        fi
    done
    if ((${#missing[@]})); then
        log "missing build dependencies: ${missing[*]}"
        log "install with:"
        log "  sudo apt-get install -y ${missing[*]}"
    else
        log "build dependencies: all present"
    fi
    if ((${#optional_missing[@]})); then
        log "optional dependencies absent: ${optional_missing[*]}"
        log "  (virglrenderer builds without them; the guest-side 3D path may need llvm-dev)"
    fi
    ((${#missing[@]} == 0))
}

installed_version() {
    local marker="${PREFIX}/.virgl-version"
    if [[ -s "$marker" ]]; then
        printf 'local\t%s\t%s\n' "${PREFIX}/lib/libvirglrenderer.so.1" "$(<"$marker")"
    fi
    local pkg lib ver
    if pkg="$(dpkg-query -W -f='${Version}' libvirglrenderer1 2>/dev/null)"; then
        for lib in /usr/lib/x86_64-linux-gnu/libvirglrenderer.so.1 /usr/lib/libvirglrenderer.so.1; do
            [[ -e "$lib" ]] || continue
            ver="${pkg%%-*}"
            printf 'distro\t%s\t%s\n' "$lib" "$ver"
        done
    fi
}

version_ge() {
    printf '%s\n%s\n' "$1" "$2" | sort -V -C && true
}

cmd_check() {
    log "== host =="
    if have qemu-system-x86_64; then
        log "qemu: $(qemu-system-x86_64 --version | head -1)"
    else
        log "qemu: not found (or not the x86_64 binary)"
    fi
    log "== virglrenderer =="
    have virglrenderer && log "virglrenderer CLI: $(command -v virglrenderer)"
    local found=0 kind lib ver
    while IFS=$'\t' read -r kind lib ver; do
        [[ -n "$lib" ]] || continue
        found=1
        log "  ${lib} (${ver}) [${kind}]"
    done < <(installed_version)
    ((found)) || log "  no libvirglrenderer.so.1 found"
    log "== device =="
    if [[ -e /dev/dri/renderD128 ]]; then
        [[ -r /dev/dri/renderD128 && -w /dev/dri/renderD128 ]] \
            && log "  /dev/dri/renderD128 readable and writable" \
            || log "  /dev/dri/renderD128 exists but is NOT accessible to this user"
    else
        log "  /dev/dri/renderD128 missing -- no GPU for the host renderer to use"
    fi
    log "== dependencies =="
    report_deps || return 1
}

cmd_build() {
    have meson   || die "meson not found; run '$0 check' for the install line"
    have ninja   || die "ninja not found; run '$0 check' for the install line"
    report_deps || die "install the missing dependencies first"

    mkdir -p "$(dirname -- "$PREFIX")" "$SRC_DIR" "$BUILD_DIR"

    local tarball="${SRC_DIR}/virglrenderer-${VIRGL_VERSION}.tar.bz2"
    if [[ ! -s "$tarball" ]]; then
        log "fetching virglrenderer ${VIRGL_VERSION}"
        curl -fL --retry 3 -o "${tarball}.part" "$VIRGL_URL" \
            || die "download failed: $VIRGL_URL"
        # A wrong path on gitlab answers 200 with a sign-in HTML page, so
        # curl succeeding proves nothing.  Decompressing is the real check.
        bzip2 -t "${tarball}.part" 2>/dev/null \
            || die "downloaded file is not a bzip2 archive -- wrong URL?
       got $(file -b "${tarball}.part")
       url: $VIRGL_URL"
        mv "${tarball}.part" "$tarball"
    fi

    # The archive unpacks to "virglrenderer-<tag>", whose exact name depends on
    # whether the tag carries the "virglrenderer-" prefix.  Discover it instead
    # of assuming, so a tag rename cannot turn into "no meson.build" here.
    local src=""
    local d
    for d in "${SRC_DIR}"/virglrenderer-"${VIRGL_VERSION}" "${SRC_DIR}"/virglrenderer-virglrenderer-"${VIRGL_VERSION}"; do
        if [[ -f "$d/meson.build" ]]; then
            src="$d"
            break
        fi
    done
    if [[ -z "$src" ]]; then
        log "unpacking"
        tar -xf "$tarball" -C "$SRC_DIR"
        for d in "${SRC_DIR}"/virglrenderer-*; do
            if [[ -f "$d/meson.build" ]]; then
                src="$d"
                break
            fi
        done
    fi
    [[ -n "$src" ]] || die "archive unpacked but no meson.build found under ${SRC_DIR}"

    log "configuring (prefix ${PREFIX})"
    # Only pass options that exist in this release.  meson treats an unknown
    # -D as a hard error, not a warning, so an option that was renamed or
    # removed upstream stops the configure outright.  valgrind=false because
    # the helper is optional; it is a boolean, so "disabled" is rejected.
    # venus=false: the Venus protocol needs host blob support, which this
    # script's QEMU command line does not set up, and its absence is not an
    # error for the virgl path we actually use.
    meson setup "$BUILD_DIR" "$src" \
        --prefix="$PREFIX" \
        --buildtype=release \
        --wipe \
        -Dvalgrind=false \
        -Dvenus=false

    log "compiling ($(nproc) jobs)"
    meson compile -C "$BUILD_DIR" -j "$(nproc)"

    log "installing"
    meson install -C "$BUILD_DIR" >/dev/null
    printf '%s\n' "$VIRGL_VERSION" > "${PREFIX}/.virgl-version"
    ldconfig 2>/dev/null || true

    local libpath
    if ! libpath="$(lib_dir)"; then
        die "build finished but no libvirglrenderer.so.1 under ${PREFIX}"
    fi
    log ""
    log "installed: ${libpath}/libvirglrenderer.so.1 -> $(readlink -f "${libpath}/libvirglrenderer.so.1")"
    log "version:   ${VIRGL_VERSION} (floor for QEMU GL passthrough is ${MIN_VIRGL})"
    log ""
    log "Now run a 3D instance. The Makefile uses this prefix automatically:"
    log "  make ARCH=riscv64 GPU_3D=1 QEMU_MEMORY=2G run-world-gui"
    log ""
    log "Falsifiable check -- the guest log must show a capset of at least"
    log "${STALE_CAPSET_BYTES} bytes. 308 bytes means the stale renderer is still"
    log "being loaded and the build did not take effect:"
    log "  tools/a20 test smoke-gpu3d-riscv64"
}

# meson installs under lib/<triplet>/ when built natively, so the lib dir is
# discovered rather than assumed.  Guessing "lib" gives an LD_LIBRARY_PATH that
# silently does nothing, and the symptom is the old library still being loaded.
lib_dir() {
    local d
    for d in "$PREFIX/lib" "$PREFIX/lib/"*"-linux-gnu" "$PREFIX/lib64"; do
        [[ -e "$d/libvirglrenderer.so.1" ]] && { printf '%s\n' "$d"; return 0; }
    done
    return 1
}

cmd_env() {
    local d
    d="$(lib_dir)" || die "no build at ${PREFIX}; run '$0 build' first"
    printf 'LD_LIBRARY_PATH=%s\n' "$d"
}

case "${1:-check}" in
    check) cmd_check ;;
    build) cmd_build ;;
    env)   cmd_env ;;
    *) die "usage: $0 {check|build|env}" ;;
esac
