#!/bin/mksh
#
# final_contest.sh — 2026 final-round automated test runner.
#
# This is deliberately separate from contest.sh, which remains the preliminary
# contest entry point.  init selects this script only when
# /a20/etc/final-eval-group is present.  The published EXT4 image is mounted
# directly at /; /a20 is only the private bootstrap FAT image.

run_final_group() {
    typeset group=$1
    typeset script="/glibc/${group}_testcode.sh"

    if [[ ! -f $script ]]; then
        print "[FINAL-EVAL][ERROR] test script not found: $script"
        return 127
    fi

    print "#### A20OS 2026 FINAL EVAL START $group ####"
    (
        PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin:/glibc:/a20
        export PATH
        # Keep A20OS's already validated shell as the script interpreter.  The
        # script and every binary it launches still see the published image as
        # the real /; this only avoids making final entry depend on GNU bash.
        cd /glibc && /a20/mksh "$script"
    )
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
    report_final_path /
    report_final_path /glibc
    report_final_path /glibc/cagent_testcode.sh
    report_final_path /glibc/buildstorm_testcode.sh

    for test_script in /glibc/*_testcode.sh; do
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
        test_script="/glibc/${group}_testcode.sh"
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
    for test_script in /glibc/*_testcode.sh; do
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
        print "[FINAL-EVAL][ERROR] no /glibc/*_testcode.sh found"
        return 127
    fi
    return $failed
}

run_buildstorm_probe() {
    typeset probe_case=

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

    cp /a20/mksh /a20-eval-shell || return
    cp /a20/buildstorm_probe.sh /a20-buildstorm-probe.sh || return
    mkdir -p /a20-probe || return
    cp /a20/a20-probe/cwd-probe /a20-probe/cwd-probe || return
    cp /a20/a20-probe/exec-pages-probe /a20-probe/exec-pages-probe || return
    cp /a20/a20-probe/shebang-probe /a20-probe/shebang-probe || return
    cp /a20/a20-probe/stage9-perf-probe /a20-probe/stage9-perf-probe || return
    cp /a20/a20-probe/liba20probe.so /a20-probe/liba20probe.so || return
    cp /a20/arch_context_stress /a20-context-stress || return
    chmod 755 /a20-eval-shell /a20-buildstorm-probe.sh \
        /a20-probe/cwd-probe /a20-probe/exec-pages-probe \
        /a20-probe/shebang-probe /a20-probe/stage9-perf-probe \
        /a20-context-stress
    chmod 644 /a20-probe/liba20probe.so

    print "#### A20OS 2026 FINAL EVAL START buildstorm-probe ####"
    if [[ -f /a20/etc/final-eval-probe-case ]]; then
        IFS= read -r probe_case </a20/etc/final-eval-probe-case
    fi
    if [[ -n $probe_case ]]; then
        print "[FINAL-EVAL] selected buildstorm probe case=$probe_case"
        /a20-eval-shell /a20-buildstorm-probe.sh --only "$probe_case"
    else
        /a20-eval-shell /a20-buildstorm-probe.sh
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
