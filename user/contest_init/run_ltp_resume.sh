#!/bin/mksh
#
# 用来快速排查哪些ltp样例会卡死
#

# ── find bin dir ────────────────────────────────────────────
typeset bin_dir=
for d in /test/*/ltp/testcases/bin; do
    [[ -d $d ]] && { bin_dir=$d; break; }
done

if [[ -z $bin_dir ]]; then
    print "[LTP-RESUME] no ltp/testcases/bin found"
    exit 1
fi

print "[LTP-RESUME] bin_dir=$bin_dir"

# ── resume point: last line in blacklist (non-comment, non-empty) ──
typeset resume_after=
for _bl in /etc/ltp_blacklist.txt /bin/etc/ltp_blacklist.txt; do
    [[ -f $_bl ]] && { resume_after=$_bl; break; }
done

typeset -i skip=1  # default: don't skip (run all)
if [[ -n $resume_after ]]; then
    typeset last=
    while IFS= read -r l; do
        [[ $l != \#* && -n $l ]] && last=$l
    done <"$resume_after"
    if [[ -n $last ]]; then
        print "[LTP-RESUME] resuming after: $last"
        skip=0  # skip until we pass the resume point
    fi
fi

# ── run ──────────────────────────────────────────────────────
typeset -i total=0 pass=0 fail=0

for bin in "$bin_dir"/*; do
    [[ -f $bin && -x $bin ]] || continue
    [[ ${bin##*/} == *.sh ]] && continue
    typeset name=${bin##*/}

    if (( ! skip )); then
        if [[ $name == "$last" ]]; then
            print "[LTP-RESUME] skipped $name (resume point)"
            skip=1
            continue
        fi
        print "[LTP-RESUME] skipping $name ..."
        continue
    fi

    (( total++ ))
    print "RUN LTP CASE $name"
    if "$bin"; then
        print "END LTP CASE $name : 0"
        (( pass++ ))
    else
        typeset rc=$?
        print "FAIL LTP CASE $name : $rc"
        (( fail++ ))
    fi
done

print "\n[LTP-RESUME] Done: $total ran, $pass passed, $fail failed"
