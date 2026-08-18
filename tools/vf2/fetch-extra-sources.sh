#!/usr/bin/env bash
set -euo pipefail

# The extra image needs these gitlinks in addition to the base user tree.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [ "${VF2_GIT_TRANSPORT:-ssh}" = ssh ]; then
    GIT_ARGS=(-c 'url.git@github.com:.insteadOf=https://github.com/')
else
    GIT_ARGS=()
fi

for path in \
	user/external/apps/fastfetch \
	user/external/apps/git \
	user/external/apps/vim \
	user/external/gcc \
	user/external/libs/zlib; do
    update_args=(--init)
    # Vim's pinned object is old enough that a shallow clone may not contain it.
    [ "$path" = user/external/apps/vim ] || update_args+=(--depth 1)
    if git "${GIT_ARGS[@]}" submodule update "${update_args[@]}" -- "$path"; then
        continue
    fi
    # The pinned Vim gitlink predates a history rewrite on the upstream
    # mirror.  Keep the build usable by checking out the current upstream
    # tree when that exact object is no longer advertised.
    if [ "$path" = user/external/apps/vim ]; then
        git -C "$path" "${GIT_ARGS[@]}" fetch --depth 1 origin master
        git -C "$path" checkout -B a20os-build FETCH_HEAD
    else
        exit 1
    fi
done

printf '%s\n' "[VF2] extra sources ready: git vim fastfetch gcc"
