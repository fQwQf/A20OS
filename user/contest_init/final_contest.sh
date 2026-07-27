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
cagent)
    run_final_group cagent || final_failed=1
    ;;
buildstorm)
    run_final_group buildstorm || final_failed=1
    ;;
*)
    print "[FINAL-EVAL][ERROR] unknown group: $final_group"
    final_failed=1
    ;;
esac

sync
poweroff
exit $final_failed
