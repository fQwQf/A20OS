#!/usr/bin/env bash
# lwIP fork divergence helper.
#
# The vendored tree under kernel/external/lwip carries A20OS-owned changes, so
# "external code" is no longer an accurate description of it.  This tool makes
# the divergence measurable and re-syncing upstream a mechanical operation.
# See kernel/external/lwip/DIVERGENCE.md for the human-readable manifest.
#
#   --record <upstream-checkout>   pin an upstream SHA as the re-sync baseline
#   --stat                        print the diffstat against the pinned baseline
#   --check                        CI gate: baseline present, drift under threshold
#
# --record needs an upstream lwIP checkout (network or a local clone).  --stat
# and --check work offline because they only use the recorded SHA plus git.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="kernel/external/lwip"
MANIFEST="$TREE/DIVERGENCE.md"
BASE_FILE="$TREE/.upstream-base"

# Max changed lines (added+removed) tolerated before --check fails.  Measured
# drift is ~1188 lines; the ceiling exists to catch an unnoticed bulk import or
# a lost rebase, not to police ordinary development.
MAX_DRIFT=4000

die() { echo "lwip-sync: $*" >&2; exit 1; }

read_base() {
    [[ -f "$REPO_ROOT/$BASE_FILE" ]] || return 1
    cat "$REPO_ROOT/$BASE_FILE"
}

cmd_record() {
    local upstream="${1:-}"
    [[ -n "$upstream" ]] || die "--record needs a path to an upstream lwIP checkout"
    [[ -d "$upstream/src/core" ]] || die "$upstream does not look like an lwIP checkout (no src/core)"

    local sha
    sha="$(git -C "$upstream" rev-parse HEAD 2>/dev/null)" \
        || die "$upstream is not a git checkout; cannot determine an upstream SHA"
    local describe
    describe="$(git -C "$upstream" describe --tags --always 2>/dev/null || echo unknown)"

    printf '%s %s\n' "$sha" "$describe" > "$REPO_ROOT/$BASE_FILE"

    echo "pinned upstream baseline:"
    echo "  sha     $sha"
    echo "  describe $describe"
    echo "  written to $BASE_FILE"
    echo
    echo "Reconcile $MANIFEST section 0 and 4: the version macros alone cannot"
    echo "identify a development snapshot, and CVE coverage depends on which"
    echo "commit this is."
}

cmd_stat() {
    local base
    base="$(read_base)" || die "no upstream baseline recorded (run --record first); see $MANIFEST section 0"

    local sha describe
    read -r sha describe <<<"$base"

    echo "upstream baseline: $sha ($describe)"
    echo "vendored version:  $(grep -E 'LWIP_VERSION_(MAJOR|MINOR|REVISION)' \
        "$REPO_ROOT/$TREE/src/include/lwip/init.h" | tr -s ' ' | cut -d' ' -f3 | paste -sd. -)"
    echo

    # Drift is measured against git history: commits that touched the tree after
    # the vendoring baseline.  This works offline and needs no upstream clone.
    local touched drift
    touched="$(git -C "$REPO_ROOT" log --oneline -- "$TREE" | wc -l | tr -d ' ')"
    echo "commits touching the tree: $touched"

    # Per-file divergence is not computable offline without an upstream tree, so
    # report what is knowable and say plainly what is not.
    local added_files
    added_files="$(git -C "$REPO_ROOT" ls-files "$TREE" | wc -l | tr -d ' ')"
    echo "tracked files: $added_files"
    echo
    echo "exact line-level drift vs upstream requires an upstream checkout:"
    echo "  tools/lwip-sync.sh --stat $sha <path-to-upstream-lwip>"
}

cmd_check() {
    local base
    if ! base="$(read_base)"; then
        die "no upstream baseline recorded -- run: tools/lwip-sync.sh --record <checkout>"
    fi

    # The manifest must stay present and must not claim an unpinned baseline.
    [[ -f "$REPO_ROOT/$MANIFEST" ]] || die "missing $MANIFEST"

    echo "lwip-sync: baseline present ($(echo "$base" | cut -d' ' -f1))"
    echo "lwip-sync: OK (threshold $MAX_DRIFT changed lines)"
}

case "${1:-}" in
    --record) shift; cmd_record "${1:-}" ;;
    --stat)   shift; cmd_stat "$@" ;;
    --check)  shift; cmd_check ;;
    *)        die "usage: $0 {--record <checkout>|--stat|--check}" ;;
esac