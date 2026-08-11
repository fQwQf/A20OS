#!/bin/mksh
#
# final_contest.sh — 2026 final-round automated test runner.
#
# This is deliberately separate from contest.sh, which remains the preliminary
# contest entry point.  init selects this script only when
# /a20/etc/final-eval-group is present.  A20OS keeps its bootstrap root alive,
# mounts the published EXT4 image at /mnt, then chroots into it for each test.

readonly FINAL_ROOT=/mnt
readonly FINAL_CHROOT=/a20/chroot
readonly FINAL_SHELL=/a20-eval-shell

check_final_root() {
    if [[ ! -d $FINAL_ROOT || ! -d $FINAL_ROOT/glibc ]]; then
        print "[FINAL-EVAL][ERROR] published rootfs is not mounted at $FINAL_ROOT"
        return 1
    fi
    if [[ ! -x $FINAL_CHROOT ]]; then
        print "[FINAL-EVAL][ERROR] missing bootstrap chroot utility: $FINAL_CHROOT"
        return 1
    fi
    if [[ ! -x /a20/mksh ]]; then
        print "[FINAL-EVAL][ERROR] missing bootstrap shell: /a20/mksh"
        return 1
    fi
    return 0
}

prepare_final_shell() {
    cp /a20/mksh "$FINAL_ROOT$FINAL_SHELL" || return
    chmod 755 "$FINAL_ROOT$FINAL_SHELL" || return
}

run_final_group() {
    typeset group=$1
    typeset script="/glibc/${group}_testcode.sh"

    check_final_root || return
    prepare_final_shell || return
    if [[ ! -f "$FINAL_ROOT$script" ]]; then
        print "[FINAL-EVAL][ERROR] test script not found: $FINAL_ROOT$script"
        return 127
    fi

    print "#### A20OS 2026 FINAL EVAL START $group ####"
    PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin \
    HOME=/root SHELL="$FINAL_SHELL" LD_LIBRARY_PATH= \
        "$FINAL_CHROOT" "$FINAL_ROOT" "$FINAL_SHELL" -c \
        'cd /glibc && exec /a20-eval-shell "$1"' \
        a20-eval-shell "$script"
    typeset -i rc=$?
    print "[FINAL-EVAL] $group runner exit=$rc"
    print "#### A20OS 2026 FINAL EVAL END $group ####"
    return $rc
}

report_final_path() {
    typeset path=$1
    typeset exists=no
    typeset directory=no
    typeset regular=no
    typeset readable=no
    typeset executable=no

    [[ -e $path ]] && exists=yes
    [[ -d $path ]] && directory=yes
    [[ -f $path ]] && regular=yes
    [[ -r $path ]] && readable=yes
    [[ -x $path ]] && executable=yes
    print "[FINAL-EVAL][PRECHECK] path=$path exists=$exists directory=$directory file=$regular readable=$readable executable=$executable"
}

run_all_final_groups() {
    typeset test_script=
    typeset group=
    typeset -i found=0
    typeset -i failed=0
    typeset -i glob_count=0

    # Preserve enough evidence to distinguish a missing/mis-mounted image,
    # direct pathname lookup failure, and getdents/glob enumeration failure.
    # Use only mksh builtins so diagnostics remain available even when the
    # published rootfs cannot execute any helper binary.
    report_final_path "$FINAL_ROOT"
    report_final_path "$FINAL_ROOT/glibc"
    report_final_path "$FINAL_ROOT/glibc/cagent_testcode.sh"
    report_final_path "$FINAL_ROOT/glibc/buildstorm_testcode.sh"

    for test_script in "$FINAL_ROOT"/glibc/*_testcode.sh; do
        [[ -f $test_script ]] || continue
        (( glob_count += 1 ))
        print "[FINAL-EVAL][PRECHECK] glob-script=$test_script"
    done
    print "[FINAL-EVAL][PRECHECK] glob-count=$glob_count"

    # The final currently defines these two score groups.  Resolve their
    # stable paths directly before relying on directory enumeration: this is
    # both a safe fallback for a getdents/glob-only failure and an opportunity
    # to earn points while retaining diagnostics for the remaining scan.
    for group in cagent buildstorm; do
        test_script="$FINAL_ROOT/glibc/${group}_testcode.sh"
        if [[ -f $test_script ]]; then
            found=1
            print "[FINAL-EVAL][PRECHECK] known-group=$group present=yes"
            run_final_group "$group" || failed=1
        else
            print "[FINAL-EVAL][PRECHECK] known-group=$group present=no"
        fi
    done

    # The published image supplies /glibc/xxxxx_testcode.sh.  Run every group
    # serially; the judge does not require a particular group order.  Known
    # groups were already run through direct lookup above, so skip duplicates.
    for test_script in "$FINAL_ROOT"/glibc/*_testcode.sh; do
        [[ -f $test_script ]] || continue
        group=${test_script##*/}
        group=${group%_testcode.sh}
        case "$group" in
        cagent|buildstorm)
            continue
            ;;
        esac
        found=1
        print "[FINAL-EVAL][PRECHECK] extra-group=$group"
        run_final_group "$group" || failed=1
    done
    if (( ! found )); then
        print "[FINAL-EVAL][ERROR] no $FINAL_ROOT/glibc/*_testcode.sh found"
        return 127
    fi
    return $failed
}

run_buildstorm_probe() {
    typeset probe_case=

    check_final_root || return

    if [[ ! -f /a20/buildstorm_probe.sh ]]; then
        print "[FINAL-EVAL][ERROR] missing /a20/buildstorm_probe.sh"
        return 127
    fi
    if [[ ! -x /a20/a20-probe/cwd-probe ||
          ! -x /a20/a20-probe/exec-pages-probe ||
          ! -x /a20/a20-probe/shebang-probe ||
          ! -x /a20/a20-probe/stage9-perf-probe ||
          ! -f /a20/a20-probe/liba20probe.so ]]; then
        print "[FINAL-EVAL][ERROR] incomplete architecture probe payload"
        return 127
    fi

    prepare_final_shell || return
    cp /a20/buildstorm_probe.sh "$FINAL_ROOT/a20-buildstorm-probe.sh" || return
    mkdir -p "$FINAL_ROOT/a20-probe" || return
    cp /a20/a20-probe/cwd-probe "$FINAL_ROOT/a20-probe/cwd-probe" || return
    cp /a20/a20-probe/exec-pages-probe "$FINAL_ROOT/a20-probe/exec-pages-probe" || return
    cp /a20/a20-probe/shebang-probe "$FINAL_ROOT/a20-probe/shebang-probe" || return
    cp /a20/a20-probe/stage9-perf-probe "$FINAL_ROOT/a20-probe/stage9-perf-probe" || return
    cp /a20/a20-probe/liba20probe.so "$FINAL_ROOT/a20-probe/liba20probe.so" || return
    cp /a20/arch_context_stress "$FINAL_ROOT/a20-context-stress" || return
    chmod 755 "$FINAL_ROOT/a20-eval-shell" \
        "$FINAL_ROOT/a20-buildstorm-probe.sh" \
        "$FINAL_ROOT/a20-probe/cwd-probe" \
        "$FINAL_ROOT/a20-probe/exec-pages-probe" \
        "$FINAL_ROOT/a20-probe/shebang-probe" \
        "$FINAL_ROOT/a20-probe/stage9-perf-probe" \
        "$FINAL_ROOT/a20-context-stress"
    chmod 644 "$FINAL_ROOT/a20-probe/liba20probe.so"

    print "#### A20OS 2026 FINAL EVAL START buildstorm-probe ####"
    if [[ -f /a20/etc/final-eval-probe-case ]]; then
        IFS= read -r probe_case </a20/etc/final-eval-probe-case
    fi
    if [[ -n $probe_case ]]; then
        print "[FINAL-EVAL] selected buildstorm probe case=$probe_case"
        "$FINAL_CHROOT" "$FINAL_ROOT" /a20-eval-shell \
            /a20-buildstorm-probe.sh --only "$probe_case"
    else
        "$FINAL_CHROOT" "$FINAL_ROOT" /a20-eval-shell \
            /a20-buildstorm-probe.sh
    fi
    typeset -i rc=$?
    print "[FINAL-EVAL] buildstorm-probe runner exit=$rc"
    print "#### A20OS 2026 FINAL EVAL END buildstorm-probe ####"
    return $rc
}

typeset final_group=
if [[ ! -f /a20/etc/final-eval-group ]]; then
    print "[FINAL-EVAL][ERROR] missing /a20/etc/final-eval-group"
    sync
    poweroff
    exit 1
fi
IFS= read -r final_group </a20/etc/final-eval-group

typeset -i final_failed=0
case "$final_group" in
all|auto)
    run_all_final_groups || final_failed=1
    ;;
cagent)
    run_final_group cagent || final_failed=1
    ;;
buildstorm)
    run_final_group buildstorm || final_failed=1
    ;;
buildstorm-probe)
    run_buildstorm_probe || final_failed=1
    ;;
*)
    print "[FINAL-EVAL][ERROR] unknown group: $final_group"
    final_failed=1
    ;;
esac

sync
poweroff
exit $final_failed
