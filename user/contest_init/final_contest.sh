#!/bin/mksh
#
# final_contest.sh — 2026 final-round automated test runner.
#
# This is deliberately separate from contest.sh, which remains the preliminary
# contest entry point.  init selects this script only when
# /bin/etc/final-eval-group is present.

run_final_group() {
    typeset group=$1
    typeset script="/glibc/${group}_testcode.sh"
    typeset chroot_bin=

    if [[ ! -f "/test$script" ]]; then
        print "[FINAL-EVAL][ERROR] test script not found: /test$script"
        return 127
    fi

    for chroot_bin in /bin/chroot /test/usr/sbin/chroot /test/usr/bin/chroot; do
        [[ -x $chroot_bin ]] && break
        chroot_bin=
    done
    if [[ -z $chroot_bin ]]; then
        print "[FINAL-EVAL][ERROR] chroot utility not found in published rootfs"
        return 127
    fi
    if ! cp /bin/mksh /test/a20-eval-shell; then
        print "[FINAL-EVAL][ERROR] failed to install evaluation shell"
        return 127
    fi
    chmod 755 /test/a20-eval-shell

    print "#### A20OS 2026 FINAL EVAL START $group ####"
    "$chroot_bin" /test /a20-eval-shell -c \
        "PATH=/glibc:/bin:/usr/bin:/sbin:/usr/sbin; export PATH; cd /glibc && exec /a20-eval-shell '$script'"
    typeset -i rc=$?
    print "[FINAL-EVAL] $group runner exit=$rc"
    print "#### A20OS 2026 FINAL EVAL END $group ####"
    return $rc
}

run_all_final_groups() {
    typeset test_script=
    typeset group=
    typeset -i found=0
    typeset -i failed=0

    # The published image supplies /glibc/xxxxx_testcode.sh.  Run every group
    # serially; the judge does not require a particular group order.
    for test_script in /test/glibc/*_testcode.sh; do
        [[ -f $test_script ]] || continue
        found=1
        group=${test_script##*/}
        group=${group%_testcode.sh}
        run_final_group "$group" || failed=1
    done
    if (( ! found )); then
        print "[FINAL-EVAL][ERROR] no /test/glibc/*_testcode.sh found"
        return 127
    fi
    return $failed
}

run_buildstorm_probe() {
    typeset chroot_bin=
    typeset probe_case=

    if [[ ! -f /bin/buildstorm_probe.sh ]]; then
        print "[FINAL-EVAL][ERROR] missing /bin/buildstorm_probe.sh"
        return 127
    fi
    if [[ ! -x /bin/a20-probe/cwd-probe ||
          ! -x /bin/a20-probe/exec-pages-probe ||
          ! -x /bin/a20-probe/shebang-probe ||
          ! -x /bin/a20-probe/stage9-perf-probe ||
          ! -f /bin/a20-probe/liba20probe.so ]]; then
        print "[FINAL-EVAL][ERROR] incomplete architecture probe payload"
        return 127
    fi
    for chroot_bin in /bin/chroot /test/usr/sbin/chroot /test/usr/bin/chroot; do
        [[ -x $chroot_bin ]] && break
        chroot_bin=
    done
    if [[ -z $chroot_bin ]]; then
        print "[FINAL-EVAL][ERROR] chroot utility not found in published rootfs"
        return 127
    fi

    cp /bin/mksh /test/a20-eval-shell || return
    cp /bin/buildstorm_probe.sh /test/a20-buildstorm-probe.sh || return
    mkdir -p /test/a20-probe || return
    cp /bin/a20-probe/cwd-probe /test/a20-probe/cwd-probe || return
    cp /bin/a20-probe/exec-pages-probe /test/a20-probe/exec-pages-probe || return
    cp /bin/a20-probe/shebang-probe /test/a20-probe/shebang-probe || return
    cp /bin/a20-probe/stage9-perf-probe /test/a20-probe/stage9-perf-probe || return
    cp /bin/a20-probe/liba20probe.so /test/a20-probe/liba20probe.so || return
    cp /bin/arch_context_stress /test/a20-context-stress || return
    chmod 755 /test/a20-eval-shell /test/a20-buildstorm-probe.sh \
        /test/a20-probe/cwd-probe /test/a20-probe/exec-pages-probe \
        /test/a20-probe/shebang-probe /test/a20-probe/stage9-perf-probe \
        /test/a20-context-stress
    chmod 644 /test/a20-probe/liba20probe.so

    print "#### A20OS 2026 FINAL EVAL START buildstorm-probe ####"
    if [[ -f /bin/etc/final-eval-probe-case ]]; then
        IFS= read -r probe_case </bin/etc/final-eval-probe-case
    fi
    if [[ -n $probe_case ]]; then
        print "[FINAL-EVAL] selected buildstorm probe case=$probe_case"
        "$chroot_bin" /test /a20-eval-shell \
            /a20-buildstorm-probe.sh --only "$probe_case"
    else
        "$chroot_bin" /test /a20-eval-shell /a20-buildstorm-probe.sh
    fi
    typeset -i rc=$?
    print "[FINAL-EVAL] buildstorm-probe runner exit=$rc"
    print "#### A20OS 2026 FINAL EVAL END buildstorm-probe ####"
    return $rc
}

typeset final_group=
if [[ ! -f /bin/etc/final-eval-group ]]; then
    print "[FINAL-EVAL][ERROR] missing /bin/etc/final-eval-group"
    sync
    poweroff
    exit 1
fi
IFS= read -r final_group </bin/etc/final-eval-group

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
