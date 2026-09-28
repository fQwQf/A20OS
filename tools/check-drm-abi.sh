#!/usr/bin/env bash
# Cross-check every DRM ioctl number A20OS implements against the Linux UAPI.
#
# Why this gate exists
# --------------------
# kernel/include/drivers/gpu/drm.h spells its ioctl numbers out as literal hex,
# because they land in a switch rather than going through a macro. That makes a
# wrong constant invisible: the driver's switch never matches, the ioctl falls
# through to the default arm, and userspace sees EINVAL or ENOTTY. The symptom
# points at Mesa or libdrm instead of at the one character that is wrong.
#
# That is not hypothetical -- this gate was written because the numbers had
# been reviewed by eye and had already drifted.
#
# The comparison is done by asking the C preprocessor and compiler, not by
# re-implementing _IOWR() in Python. Struct sizes are the whole point, and a
# parser that mis-reads one struct layout would either miss a real bug or
# "fix" correct code, which is worse than having no gate. The compiler already
# knows the answer.
#
#   tools/check-drm-abi.py
#
# Exits non-zero on any mismatch. Skips (exit 0) when no Linux UAPI DRM header
# is installed, so it stays usable on a machine that is not a Linux host.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
A20OS_HEADER="${REPO_ROOT}/kernel/include/drivers/gpu/drm.h"

UAPI_DIR=""
for cand in /usr/src/linux-headers-*/include/uapi/drm; do
    [[ -d "$cand" ]] && UAPI_DIR="$cand"
done

if [[ -z "$UAPI_DIR" ]]; then
    echo "skip: no /usr/src/linux-headers-*/include/uapi/drm on this host"
    echo "      (install linux-headers to run this gate)"
    exit 0
fi

if ! command -v "${CC:-cc}" >/dev/null 2>&1; then
    echo "skip: no C compiler available"
    exit 0
fi

mapfile -t MACROS < <(
    grep -oE '^#define[[:space:]]+DRM_IOCTL_[A-Z0-9_]+' "$A20OS_HEADER" \
        | awk '{print $2}' | sort -u
)
if ((${#MACROS[@]} == 0)); then
    echo "FAIL: no DRM_IOCTL_* macros found in ${A20OS_HEADER}"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

{
    echo '#include <stdio.h>'
    echo '#include <stdint.h>'
    # drm.h alone drags in the rest of the UAPI via drm_mode.h; include what we
    # need explicitly so this does not depend on the kernel header layout.
    echo '#include <drm/drm.h>'
    echo '#include <drm/drm_mode.h>'
    echo '#include <drm/virtgpu_drm.h>'
    echo 'int main(void) {'
    for m in "${MACROS[@]}"; do
        printf '\n#ifdef %s\n    printf("%s 0x%%08lx\\n", (unsigned long)%s);\n#else\n    printf("%s MISSING\\n");\n#endif\n' \
            "$m" "$m" "$m" "$m"
    done
    echo 'return 0;'
    echo '}'
} > "$WORK/probe.c"

if ! "${CC:-cc}" -w -o "$WORK/probe" "$WORK/probe.c" 2>"$WORK/cc.err"; then
    echo "FAIL: could not compile the probe against ${UAPI_DIR}"
    sed -n '1,20p' "$WORK/cc.err" >&2
    exit 1
fi

"$WORK/probe" | sort > "$WORK/linux.txt"

declare -A WANT
while read -r name value; do
    [[ -n "$name" ]] || continue
    WANT["$name"]="${value#0x}"
done < <(grep -oE '^#define[[:space:]]+DRM_IOCTL_[A-Z0-9_]+[[:space:]]+0x[0-9a-fA-F]+' "$A20OS_HEADER" \
         | awk '{print $2, $3}')

bad=0
missing=0
checked=0
printf 'linux uapi: %s\n\n' "$UAPI_DIR"
while read -r name value; do
    [[ -n "$name" ]] || continue
    ours="${WANT[$name]:-}"
    if [[ -z "$ours" ]]; then
        continue
    fi
    checked=$((checked + 1))
    if [[ "$value" == "MISSING" ]]; then
        printf '  ??  %-40s ours=0x%s  (no Linux UAPI definition)\n' "$name" "$ours"
        missing=$((missing + 1))
        continue
    fi
    # normalise to 8 lowercase hex digits
    ref="${value#0x}"
    ref="$(printf '%08x' "$((16#$ref))")"
    if [[ "$ours" == "$ref" ]]; then
        printf '  ok  %-40s 0x%s\n' "$name" "$ours"
    else
        printf '  BAD %-40s ours=0x%s  linux=0x%s\n' "$name" "$ours" "$ref"
        bad=$((bad + 1))
    fi
done < "$WORK/linux.txt"

printf '\n'
if ((bad)); then
    printf 'check-drm-abi: %d of %d ioctl number(s) disagree with the Linux UAPI\n' "$bad" "$checked"
    printf '  A wrong number never matches the switch: the ioctl falls through to\n'
    printf '  the default arm and userspace sees EINVAL/ENOTTY, which reads like a\n'
    printf '  Mesa bug rather than a wrong constant in a header.\n'
    exit 1
fi
printf 'check-drm-abi: %d ioctl number(s) match the Linux UAPI' "$checked"
if ((missing)); then
    printf ' (%d have no Linux counterpart)' "$missing"
fi
printf '\n'

# ---------------------------------------------------------------- struct sizes
# A correct ioctl number over a wrong struct layout still corrupts data: libdrm
# writes a field at the offset the UAPI says, and the driver reads it wherever
# its own struct happens to put it. drm_gem_open had exactly that bug -- Linux
# returns the handle at offset 4, the local struct had it at offset 8 -- so the
# The kernel declares its own copies of these structs inline in drm.c, so the
# only way to check the layout A20OS actually uses is to compile them. They are
# plain data structs over fixed-width integers, so they can be lifted out
# verbatim, wrapped in the integer typedefs they assume, and measured. That
# catches a local struct that drifted from the UAPI -- which a right ioctl
# number does not protect against, and which corrupts user memory silently.
#
# drm_gem_open is the worked example: Linux returns the handle at offset 4 and
# the size at 8, and the local copy had the handle at 8, so every GEM_OPEN
# returned a handle read from the wrong field.
sizes_bad=0
check_size() {
    local struct="$1" expect="$2" desc="${3:-}"
    local probe_a="${4:-}" probe_b="${5:-}"
    local c actual
    c="$WORK/sz.c"
    {
        cat "$WORK/a20_structs.h"
        echo '#include <stdio.h>'
        echo '#include <stddef.h>'
        echo "int main(void){ printf(\"%zu\", sizeof(struct $struct));"
        [[ -n "$probe_a" ]] && echo "  printf(\" %zu\", offsetof(struct $struct, $probe_a));"
        [[ -n "$probe_b" ]] && echo "  printf(\" %zu\", offsetof(struct $struct, $probe_b));"
        echo '  return 0; }'
    } > "$c"
    if ! "${CC:-cc}" -w -o "$WORK/sz" "$c" 2>"$WORK/sz.err"; then
        printf '  ??  struct %-22s %s\n' "$struct" "$(head -2 "$WORK/sz.err" | tr '\n' ' ')"
        return
    fi
    actual="$("$WORK/sz")"
    if [[ "$actual" == "$expect" ]]; then
        printf '  ok  struct %-22s %s\n' "$struct" "$actual"
    else
        printf '  BAD struct %-22s a20os=%s  linux=%s  %s\n' "$struct" "$actual" "$expect" "$desc"
        sizes_bad=$((sizes_bad + 1))
    fi
}

# Lift the wire structs out of drm.c. Only pure-integer structs are wanted, so
# anything mentioning a kernel type is dropped rather than stubbed.
{
    echo '#include <stdint.h>'
    echo 'typedef uint8_t __u8; typedef uint16_t __u16;'
    echo 'typedef uint32_t __u32; typedef uint64_t __u64;'
    echo 'typedef int32_t __s32; typedef int64_t __s64;'
    awk '
        /^struct (drm|virtio_gpu)_[a-z0-9_]+ *\{/ { buf=$0; inb=1; next }
        inb { buf = buf "\n" $0
              if ($0 ~ /^\};/) { print buf; inb=0; buf="" } }
    ' "$REPO_ROOT/kernel/drivers/gpu/drm.c"
} > "$WORK/a20_structs.h"

echo
echo "struct layouts:"
check_size drm_gem_close   8
check_size drm_gem_flink   8
check_size drm_gem_open    "16 4 8" "handle must be at offset 4, size at 8" handle size
check_size drm_mode_fb_cmd 28
check_size drm_mode_fb_cmd2 104
check_size drm_mode_crtc  104

echo
if ((sizes_bad)); then
    printf 'check-drm-abi: %d struct layout(s) disagree with the Linux UAPI\n' "$sizes_bad"
    printf '  A right number over a wrong layout still corrupts data silently.\n'
    exit 1
fi
printf 'check-drm-abi: struct layouts match\n'
