#!/bin/sh
# Run every script-driven host harness in tools/.
#
# Each of these harnesses needs something the generic single-file pattern rule
# in tools/mk/targets-gates.mk cannot supply (a module's Rust object, a per-arch
# include path, or a generated header), so the harness itself lives in its own
# tools/test-*-host.sh script.  Keeping them out of HOST_TESTS_SRC without
# running them anywhere would have quietly dropped that coverage, which is what
# this target exists to prevent.
#
# The scripts run through their own shebang: several are bash and use arrays
# or string substitution that /bin/sh rejects.
#
# Exit status 75 from a script is EX_TEMPFAIL: the repository's shared build
# slot was busy.  That is not a failure of the harness, so it is retried a
# bounded number of times; every other non-zero status is reported.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
evidence=${EVIDENCE_DIR:-"$repo/.kernel-build/host-evidence"}/scripts
# Each of the 76 scripts writes a timestamped run directory, so keeping them
# would add hundreds of megabytes to the build tree on every gate run.  This
# subdirectory belongs to this runner; the caller-supplied parent is left alone.
rm -rf "$evidence"
mkdir -p "$evidence"

attempts=6
failed=0
count=0

for script in "$repo"/tools/host-tests/test-*-host.sh \
              "$repo"/tools/tests/test-rv32-sync-abi.sh \
              "$repo"/tools/tests/test_eevdf_math_host.sh; do
    case "$(basename "$script")" in
        # Driven by their own make targets with an explicit argument, so
        # running them here too would either duplicate the run or use the
        # wrong one.
        test-nat-rewrite-host.sh|test-signal-core-host.sh) continue ;;
    esac
    name=$(basename "$script" .sh)
    count=$((count + 1))
    n=0
    while :; do
        n=$((n + 1))
        rc=0
        build_dir=
        case "$name" in
            test-rv32-sync-abi|test_eevdf_math_host)
                build_dir="$evidence/$name/build" ;;
        esac
        if [ -n "$build_dir" ]; then
            BUILD_DIR="$build_dir" EVIDENCE_DIR="$evidence/$name" \
                timeout 300 "$script" >"$evidence/$name.log" 2>&1 || rc=$?
        else
            EVIDENCE_DIR="$evidence/$name" \
                timeout 300 "$script" >"$evidence/$name.log" 2>&1 || rc=$?
        fi
        if [ "$rc" -eq 75 ] && [ "$n" -lt "$attempts" ]; then
            sleep 10
            continue
        fi
        break
    done
    if [ "$rc" -ne 0 ]; then
        printf 'check-host-scripts: %s FAILED (status %s, attempt %s)\n' \
               "$name" "$rc" "$n" >&2
        tail -20 "$evidence/$name.log" >&2
        failed=$((failed + 1))
    else
        printf 'check-host-scripts: %s PASS\n' "$name"
    fi
done

if [ "$failed" -ne 0 ]; then
    printf 'check-host-scripts: %s of %s harnesses failed\n' \
           "$failed" "$count" >&2
    exit 1
fi
printf 'check-host-scripts: %s harnesses PASS\n' "$count"
