#!/usr/bin/env bash
set -euo pipefail

# fastfetch is the last gitlink in the tree.  It is the one user program still
# compiled from vendored source, because it is linked into the FAT32 root that
# boots before any Alpine world is mounted; every other program the extra disk
# used to carry (vim, git, zlib, gcc, binutils, musl-cross-make, curl, rust,
# Lamina1) now comes from an apk world instead -- see tools/targets-extra.mk
# for the retired targets and docs/packaging/images.md for the world list.
#
# GitHub uses SSH by default; set VF2_GIT_TRANSPORT=https on hosts without a
# configured SSH key.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [ "${VF2_GIT_TRANSPORT:-ssh}" = ssh ]; then
    GIT_ARGS=(-c 'url.git@github.com:.insteadOf=https://github.com/')
else
    GIT_ARGS=()
fi

path=user/external/apps/fastfetch
if ! git "${GIT_ARGS[@]}" submodule update --init --depth 1 -- "$path"; then
    exit 1
fi

printf '%s\n' "[VF2] gitlink ready: fastfetch"