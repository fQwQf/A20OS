#!/usr/bin/env bash
# Run a command with a working headless virgl display for QEMU.
#
# Why this exists
# ---------------
# GPU_3D=1 makes A20OS attach a virgl-capable virtio-gpu, but the renderer runs
# host-side: QEMU dlopen()s libvirglrenderer and hands it the guest's GL command
# stream. If the host cannot give it a GL context, QEMU does not fail loudly --
# it hands the guest a 2D-only device, so every 3D conclusion is measured
# against nothing and DRM_IOCTL_VIRTGPU_GET_CAPS answers 0x1205. That silent
# degradation is what made this look like a guest bug for a long time.
#
# Getting a context on a headless host needs three things, and the first two are
# not obvious:
#
#  1. The EGL vendor must be Mesa. On a host with both a proprietary and an open
#     GPU driver, glvnd picks the proprietary one by default, and that EGL has no
#     device/surfaceless platform virgl can use.
#  2. Mesa enumerates *every* /dev/dri node. The proprietary GPU's node fails
#     eglInitialize under Mesa ("gbm device using incorrect/incompatible
#     backend"), and QEMU stops at that first failure instead of falling through
#     to the nodes that work. So the other nodes have to be hidden.
#  3. egl-headless needs gl=on explicitly, or QEMU refuses to create the device
#     with "the display backend does not have OpenGL support enabled".
#
# libvirglrenderer also has to be new enough for the host's Mesa. Distributions
# still ship 1.1.0 (2020) on some releases; use tools/build-virglrenderer.sh.
#
# Usage
#   tools/with-virgl-display.sh tools/a20 test smoke-gpu3d-riscv64
#   tools/with-virgl-display.sh make run-gui-x86_64
#
# Environment
#   VIRGL_RENDER_NODE / VIRGL_CARD_NODE   which node to keep visible
#                                         (default: the first /dev/dri/renderD*)
#   VIRGL_PREFIX                          virglrenderer install prefix
#   A20_VIRGL_DEBUG=1                     list the visible nodes and exit

set -euo pipefail

MESA_VENDOR_JSON="${MESA_VENDOR_JSON:-/usr/share/glvnd/egl_vendor.d/50_mesa.json}"
RENDER_NODE="${VIRGL_RENDER_NODE:-}"
CARD_NODE="${VIRGL_CARD_NODE:-}"

if [[ -z "$RENDER_NODE" ]]; then
    for n in /dev/dri/renderD*; do
        [[ -e "$n" ]] || continue
        RENDER_NODE="$n"
        break
    done
fi
if [[ ! -e "${RENDER_NODE:-}" ]]; then
    echo "error: no /dev/dri/renderD* node found; a GPU or software rasteriser is required" >&2
    exit 1
fi
# The matching card node is what udev/libinput name, and Mesa is happier with
# the pair present, but the render node alone is enough to make a context.
if [[ -z "$CARD_NODE" ]]; then
    local_card="$(basename "$RENDER_NODE" | sed 's/renderD/card/')"
    [[ -e "/dev/dri/$local_card" ]] && CARD_NODE="/dev/dri/$local_card"
fi

if [[ "${A20_VIRGL_DEBUG:-0}" == "1" ]]; then
    echo "virgl display: keeping $RENDER_NODE${CARD_NODE:+ $CARD_NODE}, hiding the rest"
    echo "egl vendor:    $MESA_VENDOR_JSON"
    exit 0
fi

if [[ ! -r "$MESA_VENDOR_JSON" ]]; then
    echo "error: $MESA_VENDOR_JSON not found; this host has no Mesa EGL vendor JSON." >&2
    echo "       Point MESA_VENDOR_JSON at the right file for this distribution." >&2
    exit 1
fi

if [[ $# -eq 0 ]]; then
    echo "usage: tools/with-virgl-display.sh <command> [args...]" >&2
    exit 2
fi

# User namespaces let an unprivileged user build the private /dev/dri view that
# hiding the other nodes requires. Not every kernel/container setup allows them;
# when it is unavailable, say so rather than silently running without a
# context, because that is the failure this script exists to prevent.
if ! unshare -Urm --propagation private true 2>/dev/null; then
    cat >&2 <<EOF
error: cannot create a user+mount namespace, which is how the other /dev/dri
       nodes get hidden.

Without that, Mesa's eglInitialize fails on the proprietary GPU's node and QEMU
aborts before reaching a device that works, so the guest silently gets a
2D-only device. Enable unprivileged user namespaces
(/proc/sys/kernel/unprivileged_userns_clone and
/proc/sys/user/max_user_namespaces), or run as root so the mount can be done
directly.
EOF
    exit 1
fi

VIRGL_LIBDIR="${VIRGL_PREFIX:-}"
if [[ -z "$VIRGL_LIBDIR" ]]; then
    for d in tools/virgl/install/lib/*-linux-gnu tools/virgl/install/lib; do
        [[ -e "$d/libvirglrenderer.so.1" ]] && { VIRGL_LIBDIR="$d"; break; }
    done
fi

exec unshare -Urm --propagation private bash -c '
  set -euo pipefail
  render_node=$1; card_node=$2; mesa_json=$3; virgl_lib=$4; shift 4
  shadow=$(mktemp -d)
  # Bind the real directory aside first: once /dev/dri is shadowed, the originals
  # are only reachable through this copy.
  mount --bind /dev/dri "$shadow"
  mount -t tmpfs none /dev/dri
  for n in "$render_node" ${card_node:+"$card_node"}; do
    base=$(basename "$n")
    : > "/dev/dri/$base"
    mount --bind "$shadow/$base" "/dev/dri/$base"
  done
  export __EGL_VENDOR_LIBRARY_FILENAMES="$mesa_json"
  if [ -n "$virgl_lib" ]; then
    export LD_LIBRARY_PATH="$virgl_lib:${LD_LIBRARY_PATH:-}"
  fi
  exec "$@"
' _ "$RENDER_NODE" "$CARD_NODE" "$MESA_VENDOR_JSON" "$VIRGL_LIBDIR" "$@"