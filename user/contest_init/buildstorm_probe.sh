#!/bin/mksh
#
# BuildStorm staged diagnostic probe.
#
# This script runs inside the untouched published rootfs through the same
# chroot boundary as the official final-round scripts.  It deliberately does
# not edit or source buildstorm_testcode.sh.

export PATH=/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/root
export RUSTUP_HOME=/root/.rustup
export CARGO_HOME=/root/.cargo
export RUSTUP_TOOLCHAIN=nightly-2026-05-28
export CARGO_NET_OFFLINE=true

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /tmp

typeset arch=
typeset toolchain=
arch=$(uname -m 2>/dev/null)
case "$arch" in
riscv64)
    toolchain=/root/.rustup/toolchains/nightly-2026-05-28-riscv64gc-unknown-linux-gnu
    ;;
loongarch64)
    toolchain=/root/.rustup/toolchains/nightly-2026-05-28-loongarch64-unknown-linux-gnu
    ;;
*)
    print "[BUILDSTORM-PROBE][FATAL] unsupported architecture: $arch"
    exit 2
    ;;
esac

typeset -i failures=0
typeset -i total=0

lifetime_snapshot() {
    typeset label=$1
    if [[ -w /proc/sys/vm/drop_caches ]]; then
        sync || {
            print "[BUILDSTORM-PROBE][LIFETIME][$label] sync failed"
            return 1
        }
        print 3 >/proc/sys/vm/drop_caches || {
            print "[BUILDSTORM-PROBE][LIFETIME][$label] drop_caches failed"
            return 1
        }
    fi
    print "[BUILDSTORM-PROBE][LIFETIME][$label] begin"
    if [[ -r /proc/a20/task_lifetime ]]; then
        cat /proc/a20/task_lifetime
    else
        print "[BUILDSTORM-PROBE][LIFETIME][$label] unavailable"
    fi
    print "[BUILDSTORM-PROBE][LIFETIME][$label] end"
}

run_case() {
    typeset name=$1
    typeset -i rc=0
    typeset -i case_timeout=180

    case "$name" in
    stage2-*-100|stage3-*-100|stage4-*|stage5-*|stage6-*|stage9-*) case_timeout=900 ;;
    stage7-*) case_timeout=28800 ;;
    esac

    (( total++ ))
    print "#### BUILDSTORM PROBE START $name ####"
    print "[BUILDSTORM-PROBE] case=$name arch=$arch phase=2"
    lifetime_snapshot "$name:before"
    /usr/bin/timeout "$case_timeout" /a20-eval-shell \
        /a20-buildstorm-probe.sh --case "$name"
    rc=$?
    lifetime_snapshot "$name:after"
    print "[BUILDSTORM-PROBE] case=$name rc=$rc"
    print "#### BUILDSTORM PROBE END $name rc=$rc ####"
    (( rc == 0 )) || (( failures++ ))
    return 0
}

probe_static_elf() {
    /a20-eval-shell -c 'print BUILDSTORM_PROBE_STATIC_ELF_OK'
}

probe_cwd_root() {
    cd / || return
    /a20-probe/cwd-probe
}

probe_cwd_glibc() {
    cd /glibc || return
    /a20-probe/cwd-probe &&
        /bin/pwd
}

probe_cwd_deep() {
    typeset base=/tmp/a20-probe-cwd
    rm -rf "$base"
    mkdir -p "$base/a/b/c" || return
    cd "$base/a/b/c" || return
    /a20-probe/cwd-probe &&
        /bin/pwd
    typeset -i rc=$?
    cd /
    rm -rf "$base"
    return $rc
}

probe_proxy_rustc() {
    /root/.cargo/bin/rustc --version
}

probe_proxy_cargo() {
    /root/.cargo/bin/cargo --version
}

probe_real_rustc_bare() {
    "$toolchain/bin/rustc" --version
}

probe_real_rustc() {
    LD_LIBRARY_PATH="$toolchain/lib" "$toolchain/bin/rustc" --version
}

probe_real_cargo() {
    "$toolchain/bin/cargo" --version
}

probe_directory_visibility() {
    typeset base=/tmp/a20-probe-dir
    rm -rf "$base"
    mkdir -p "$base/a/b/c" || return
    /usr/bin/stat "$base/a/b/c" || return
    print immediate-open >"$base/a/b/c/file" || return
    /bin/mv "$base/a/b/c/file" "$base/a/b/c/renamed" || return
    /usr/bin/stat "$base/a/b/c/renamed" || return
    /bin/rm "$base/a/b/c/renamed" || return
    rmdir "$base/a/b/c" "$base/a/b" "$base/a" "$base"
}

probe_stage2_directory_100() {
    typeset base=/tmp/a20-stage2-directory-100
    typeset -i round=1
    rm -rf "$base"
    while (( round <= 100 )); do
        typeset leaf="$base/round-$round/a/b/c"
        mkdir -p "$leaf" || return
        cd "$leaf" || return
        /a20-probe/cwd-probe >/dev/null || return
        cd / || return
        print immediate-open >"$leaf/before" || return
        /usr/bin/stat "$leaf/before" >/dev/null || return
        /bin/mv "$leaf/before" "$leaf/after" || return
        /usr/bin/stat "$leaf/after" >/dev/null || return
        /bin/rm "$leaf/after" || return
        rm -rf "$base/round-$round" || return
        if (( round % 10 == 0 )); then
            print "BUILDSTORM_STAGE2_DIRECTORY progress=$round/100"
        fi
        (( round++ ))
    done
    rmdir "$base" || return
    print "BUILDSTORM_STAGE2_DIRECTORY_100 ok"
}

probe_stage2_proxy_rustc_100() {
    typeset -i round=1
    while (( round <= 100 )); do
        /root/.cargo/bin/rustc --version >/tmp/a20-rustc-version || return
        if (( round % 10 == 0 )); then
            print "BUILDSTORM_STAGE2_RUSTC progress=$round/100"
        fi
        (( round++ ))
    done
    cat /tmp/a20-rustc-version
    rm -f /tmp/a20-rustc-version
    print "BUILDSTORM_STAGE2_RUSTC_100 ok"
}

probe_stage2_proxy_cargo_100() {
    typeset -i round=1
    while (( round <= 100 )); do
        /root/.cargo/bin/cargo --version >/tmp/a20-cargo-version || return
        if (( round % 10 == 0 )); then
            print "BUILDSTORM_STAGE2_CARGO progress=$round/100"
        fi
        (( round++ ))
    done
    cat /tmp/a20-cargo-version
    rm -f /tmp/a20-cargo-version
    print "BUILDSTORM_STAGE2_CARGO_100 ok"
}

probe_dynamic_pages() {
    LD_LIBRARY_PATH=/a20-probe /a20-probe/exec-pages-probe
}

probe_stage3_dynamic_pages_100() {
    typeset -i round=1
    while (( round <= 100 )); do
        LD_LIBRARY_PATH=/a20-probe /a20-probe/exec-pages-probe \
            >/tmp/a20-dynamic-pages || return
        if (( round % 10 == 0 )); then
            print "BUILDSTORM_STAGE3_DYNAMIC progress=$round/100"
        fi
        (( round++ ))
    done
    cat /tmp/a20-dynamic-pages
    rm -f /tmp/a20-dynamic-pages
    print "BUILDSTORM_STAGE3_DYNAMIC_100 ok"
}

probe_rustc_hello() {
    typeset base=/tmp/a20-probe-rustc
    rm -rf "$base"
    mkdir "$base" || return
    print 'fn main() { println!("Hello, world!"); }' >"$base/hello.rs"
    LD_LIBRARY_PATH="$toolchain/lib" \
        "$toolchain/bin/rustc" "$base/hello.rs" -o "$base/hello" || return
    "$base/hello"
}

probe_cargo_minibuild_j1() {
    typeset base=/tmp/a20-probe-cargo-j1
    rm -rf "$base"
    /root/.cargo/bin/cargo new --vcs none "$base" || return
    (cd "$base" && CARGO_BUILD_JOBS=1 /root/.cargo/bin/cargo build) || return
    "$base/target/debug/a20-probe-cargo-j1"
}

probe_cargo_minibuild_default() {
    typeset base=/tmp/a20-probe-cargo-default
    rm -rf "$base"
    /root/.cargo/bin/cargo new --vcs none "$base" || return
    (cd "$base" && /root/.cargo/bin/cargo build) || return
    "$base/target/debug/a20-probe-cargo-default"
}

probe_stage4_cargo_minibuild() {
    typeset base=/tmp/a20-stage4-cargo
    typeset direct=/tmp/a20-stage4-direct
    rm -rf "$base" "$direct"
    mkdir "$direct" || return
    /a20-context-stress || return
    print 'fn main() { println!("Hello, world!"); }' >"$direct/hello.rs"
    print "BUILDSTORM_STAGE4 arch=$arch cores=$(/usr/bin/nproc)"
    /root/.cargo/bin/rustc "$direct/hello.rs" -o "$direct/hello" || return
    "$direct/hello" || return
    /root/.cargo/bin/cargo new --vcs none "$base" || return
    (cd "$base" && /root/.cargo/bin/cargo build -vv) || return
    "$base/target/debug/a20-stage4-cargo" || return
    print "BUILDSTORM_STAGE4_CARGO_MINIBUILD ok"
}

probe_stage5_official_minibuild() {
    typeset base=/tmp/minibuild
    rm -rf "$base"

    unset RUSTC LD_LIBRARY_PATH CARGO_BUILD_JOBS
    /root/.cargo/bin/rustc --version || return
    /root/.cargo/bin/cargo --version || return
    print "BUILDSTORM_TOOLCHAIN ok"

    /root/.cargo/bin/cargo new --vcs none "$base" || return
    (cd "$base" && /root/.cargo/bin/cargo build) || return
    "$base/target/debug/minibuild" || return
    print "BUILDSTORM_MINIBUILD ok"

    rm -rf "$base" || return
}

probe_stage6_ext4_dir_tail() {
    typeset base=/work/a20-stage6-ext4-dir-tail
    typeset name=
    typeset -i index=0

    rm -rf "$base"
    mkdir "$base" || return

    # Four-character names occupy 12-byte ext4 directory records.  A fresh
    # second block holds one record followed by a free record; 340 further
    # insertions leave only four bytes at the block tail.  The next mkdir must
    # either be placed in a new block or fail explicitly, never report success
    # while leaving an unreachable inode.
    while (( index < 680 )); do
        name=$(printf 'f%03d' "$index")
        : >"$base/$name" || return
        if (( index % 100 == 99 )); then
            print "BUILDSTORM_STAGE6_EXT4_DIR_TAIL progress=$((index + 1))/680"
        fi
        (( index++ ))
    done

    mkdir "$base/f680" || return
    if [[ ! -d "$base/f680" ]]; then
        print "BUILDSTORM_STAGE6_EXT4_DIR_TAIL lookup-missing name=f680"
        return 1
    fi
    print invoked >"$base/f680/invoked.timestamp" || return
    /usr/bin/stat "$base/f680/invoked.timestamp" || return
    sync || return
    print "BUILDSTORM_STAGE6_EXT4_DIR_TAIL ok entries=681"
    rm -rf "$base" || return
}

stage6_helper_snapshot() {
    typeset label=$1
    typeset helper=$2
    typeset mode=missing
    typeset bytes=0
    typeset executable=no

    if [[ -f $helper ]]; then
        mode=$(/usr/bin/stat -c '%a' "$helper" 2>/dev/null) || return
        bytes=$(/usr/bin/wc -c <"$helper") || return
        [[ -x $helper ]] && executable=yes
    fi

    print "STAGE6_META tg_xtask_${label}_path=$helper"
    print "STAGE6_META tg_xtask_${label}_mode=$mode"
    print "STAGE6_META tg_xtask_${label}_bytes=$bytes"
    print "STAGE6_META tg_xtask_${label}_executable=$executable"

    [[ -f $helper && $bytes -gt 0 && $executable == yes ]]
}

probe_stage6_precompiled_helper() {
    typeset worktree=/work/tgoskits
    typeset helper="$worktree/target/debug/tg-xtask"
    typeset output=/work/a20-stage6-precompiled-helper.out
    typeset axtgt=
    typeset t0=
    typeset t1=
    typeset elapsed=0
    typeset -i rc=1

    case "$arch" in
    riscv64) axtgt=riscv64gc-unknown-linux-musl ;;
    loongarch64) axtgt=loongarch64-unknown-linux-musl ;;
    *) return 2 ;;
    esac

    cd "$worktree" || return
    print "STAGE6_META tg_xtask_timing_scope=untimed_helper_check"
    stage6_helper_snapshot before "$helper" || return

    typeset component=
    for component in .fingerprint deps build; do
        if [[ -d "$worktree/target/debug/$component" ]]; then
            print "STAGE6_META tg_xtask_cache_${component#.}=present"
        else
            print "STAGE6_META tg_xtask_cache_${component#.}=missing"
            return 1
        fi
    done

    rm -rf -- "$worktree/target/$axtgt" || return
    if [[ -e "$worktree/target/$axtgt" ]]; then
        print "STAGE6_META tg_xtask_target_cleanup=failed"
        return 1
    fi
    print "STAGE6_META tg_xtask_target_cleanup=$worktree/target/$axtgt"

    rm -f -- "$output" || return
    unset RUSTC LD_LIBRARY_PATH CARGO_BUILD_JOBS
    t0=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    cargo build -p tg-xtask >"$output" 2>&1
    rc=$?
    t1=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    elapsed=$(/usr/bin/awk \
        "BEGIN{printf \"%.2f\", (\"$t1\"+0)-(\"$t0\"+0)}" \
        2>/dev/null)
    [[ -n $elapsed ]] || elapsed=0

    print -- "----- stage6 precompiled tg-xtask stdout/stderr begin -----"
    cat "$output"
    print -- "----- stage6 precompiled tg-xtask stdout/stderr end -----"
    print "STAGE6_META tg_xtask_build_output=$output"
    print "STAGE6_META tg_xtask_build_rc=$rc"
    print "STAGE6_META tg_xtask_build_elapsed_s=$elapsed"
    (( rc == 0 )) || return $rc

    stage6_helper_snapshot after "$helper" || return
    print "BUILDSTORM_STAGE6_PRECOMPILED_HELPER ok"
}

probe_stage7_shebang_exec() {
    /a20-probe/shebang-probe
}

probe_stage7_parallel_rustc() {
    typeset base=/tmp/a20-stage7-rustc-j8
    typeset source="$base/hello.rs"
    typeset pids=
    typeset -i round=1
    typeset -i worker=1
    typeset -i rc=0

    rm -rf -- "$base" || return
    mkdir -p "$base" || return
    print 'fn main() { println!("stage7 rustc parallel probe"); }' >"$source" || return
    print "STAGE7_RUSTC_META cores=$(/usr/bin/nproc) workers=8 rounds=4"

    while (( round <= 4 )); do
        pids=
        worker=1
        while (( worker <= 8 )); do
            LD_LIBRARY_PATH="$toolchain/lib" "$toolchain/bin/rustc" \
                "$source" -o "$base/hello-$round-$worker" \
                >"$base/rustc-$round-$worker.log" 2>&1 &
            pids="$pids $!"
            (( worker++ ))
        done

        worker=1
        for pid in $pids; do
            wait "$pid" || {
                print "STAGE7_RUSTC_FAIL round=$round worker=$worker rc=$?"
                rc=1
            }
            (( worker++ ))
        done
        if (( rc != 0 )); then
            cat "$base"/rustc-*.log
            return $rc
        fi

        worker=1
        while (( worker <= 8 )); do
            "$base/hello-$round-$worker" >/dev/null || return
            (( worker++ ))
        done
        rm -f -- "$base"/hello-* "$base"/rustc-*.log || return
        print "BUILDSTORM_STAGE7_RUSTC_J8 progress=$round/4"
        (( round++ ))
    done

    print "BUILDSTORM_STAGE7_RUSTC_J8 ok compiles=32"
    rm -rf -- "$base" || return
}

probe_stage7_parallel_llvm() {
    typeset base=/tmp/a20-stage7-rustc-llvm-j8
    typeset source="$base/heavy.rs"
    typeset pids=
    typeset -i i=0
    typeset -i round=1
    typeset -i worker=1
    typeset -i rc=0

    rm -rf -- "$base" || return
    mkdir -p "$base" || return
    {
        print '#![allow(dead_code)]'
        i=0
        while (( i < 3000 )); do
            print "#[inline(never)] fn f$i(mut x: u64) -> u64 { for j in 0..32 { x = x.rotate_left(((j + $i) & 63) as u32) ^ x.wrapping_mul(0x9e3779b97f4a7c15); } x }"
            (( i++ ))
        done
        print 'fn main() { let mut x = 1u64;'
        i=0
        while (( i < 3000 )); do
            print "x = f$i(x);"
            (( i++ ))
        done
        print 'println!("{}", x); }'
    } >"$source" || return
    print "STAGE7_LLVM_META cores=$(/usr/bin/nproc) workers=8 rounds=2 functions=3000 codegen_units=16 bytes=$(wc -c <"$source")"

    while (( round <= 2 )); do
        pids=
        worker=1
        while (( worker <= 8 )); do
            LD_LIBRARY_PATH="$toolchain/lib" "$toolchain/bin/rustc" \
                -C opt-level=3 -C codegen-units=16 "$source" \
                -o "$base/heavy-$round-$worker" \
                >"$base/rustc-$round-$worker.log" 2>&1 &
            pids="$pids $!"
            (( worker++ ))
        done

        worker=1
        for pid in $pids; do
            wait "$pid" || {
                print "STAGE7_LLVM_FAIL round=$round worker=$worker rc=$?"
                rc=1
            }
            (( worker++ ))
        done
        if (( rc != 0 )); then
            cat "$base"/rustc-*.log
            return $rc
        fi
        rm -f -- "$base"/heavy-* "$base"/rustc-*.log || return
        print "BUILDSTORM_STAGE7_LLVM_J8 progress=$round/2"
        (( round++ ))
    done

    print "BUILDSTORM_STAGE7_LLVM_J8 ok compiles=16"
    rm -rf -- "$base" || return
}

stage7_helper_snapshot() {
    typeset label=$1
    typeset helper=$2
    typeset mode=missing
    typeset bytes=0
    typeset executable=no

    if [[ -f $helper ]]; then
        mode=$(/usr/bin/stat -c '%a' "$helper" 2>/dev/null) || return
        bytes=$(/usr/bin/wc -c <"$helper") || return
        [[ -x $helper ]] && executable=yes
    fi

    print "STAGE7_META stage7_helper_${label}_path=$helper"
    print "STAGE7_META stage7_helper_${label}_mode=$mode"
    print "STAGE7_META stage7_helper_${label}_bytes=$bytes"
    print "STAGE7_META stage7_helper_${label}_executable=$executable"

    [[ -f $helper && $bytes -gt 0 && $executable == yes ]]
}

probe_stage7_full_build() {
    typeset name=$1
    typeset worktree=/work/tgoskits
    typeset helper="$worktree/target/debug/tg-xtask"
    typeset minibuild=/tmp/minibuild
    typeset helper_output=
    typeset compile_output=
    typeset axarch=
    typeset axtgt=
    typeset jobs_label=
    typeset artifact=
    typeset artifact_sha=missing
    typeset t0=
    typeset t1=
    typeset helper_elapsed=0
    typeset compile_elapsed=0
    typeset bytes=0
    typeset -i jobs=0
    typeset -i helper_rc=1
    typeset -i compile_rc=1

    case "$name" in
    stage7-full-j1) jobs=1; jobs_label=1 ;;
    stage7-full-j2) jobs=2; jobs_label=2 ;;
    stage7-full-j4) jobs=4; jobs_label=4 ;;
    stage7-full-j8) jobs=8; jobs_label=8 ;;
    stage7-full-default) jobs=0; jobs_label=default ;;
    *) return 2 ;;
    esac
    case "$arch" in
    riscv64)
        axarch=riscv64
        axtgt=riscv64gc-unknown-linux-musl
        ;;
    loongarch64)
        axarch=loongarch64
        axtgt=loongarch64-unknown-linux-musl
        ;;
    *) return 2 ;;
    esac

    helper_output="/work/a20-${name}-helper.out"
    compile_output="/work/a20-${name}-compile.out"
    print "STAGE7_META stage7_case=$name"
    print "STAGE7_META stage7_arch=$axarch"
    print "STAGE7_META stage7_cargo_jobs=$jobs_label"
    print "STAGE7_META stage7_guest_nproc=$(/usr/bin/nproc)"
    print "STAGE7_META stage7_compile_command=cargo_xtask_arceos_build"

    unset RUSTC LD_LIBRARY_PATH CARGO_BUILD_JOBS
    /root/.cargo/bin/rustc --version || return
    /root/.cargo/bin/cargo --version || return
    print "BUILDSTORM_TOOLCHAIN ok"

    rm -rf -- "$minibuild" || return
    /root/.cargo/bin/cargo new --vcs none "$minibuild" || return
    (cd "$minibuild" && /root/.cargo/bin/cargo build) || return
    [[ "$("$minibuild/target/debug/minibuild")" == "Hello, world!" ]] || return
    print "BUILDSTORM_MINIBUILD ok"
    rm -rf -- "$minibuild" || return

    cd "$worktree" || return
    stage7_helper_snapshot before "$helper" || return
    typeset component=
    for component in .fingerprint deps build; do
        if [[ -d "$worktree/target/debug/$component" ]]; then
            print "STAGE7_META stage7_helper_cache_${component#.}=present"
        else
            print "STAGE7_META stage7_helper_cache_${component#.}=missing"
            return 1
        fi
    done
    rm -rf -- "$worktree/target/$axtgt" || return
    if [[ -e "$worktree/target/$axtgt" ]]; then
        print "STAGE7_META stage7_target_cleanup=failed"
        return 1
    fi
    print "STAGE7_META stage7_target_cleanup=$worktree/target/$axtgt"

    rm -f -- "$helper_output" "$compile_output" || return
    t0=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    /root/.cargo/bin/cargo build -p tg-xtask >"$helper_output" 2>&1
    helper_rc=$?
    t1=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    helper_elapsed=$(/usr/bin/awk \
        "BEGIN{printf \"%.2f\", (\"$t1\"+0)-(\"$t0\"+0)}" \
        2>/dev/null)
    [[ -n $helper_elapsed ]] || helper_elapsed=0
    print -- "----- stage7 helper stdout/stderr begin -----"
    cat "$helper_output"
    print -- "----- stage7 helper stdout/stderr end -----"
    print "STAGE7_META stage7_helper_output=$helper_output"
    print "STAGE7_META stage7_helper_build_rc=$helper_rc"
    print "STAGE7_META stage7_helper_elapsed_s=$helper_elapsed"
    (( helper_rc == 0 )) || return $helper_rc
    stage7_helper_snapshot after "$helper" || return

    t0=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    print "STAGE7_META stage7_compile_start_uptime_s=$t0"
    if (( jobs > 0 )); then
        CARGO_BUILD_JOBS=$jobs /usr/bin/timeout 14400 \
            /root/.cargo/bin/cargo xtask arceos build \
            -p arceos-helloworld --arch "$axarch" \
            >"$compile_output" 2>&1
    else
        unset CARGO_BUILD_JOBS
        /usr/bin/timeout 14400 /root/.cargo/bin/cargo xtask arceos build \
            -p arceos-helloworld --arch "$axarch" \
            >"$compile_output" 2>&1
    fi
    compile_rc=$?
    t1=$(/usr/bin/cut -d' ' -f1 /proc/uptime 2>/dev/null) || return
    compile_elapsed=$(/usr/bin/awk \
        "BEGIN{printf \"%.2f\", (\"$t1\"+0)-(\"$t0\"+0)}" \
        2>/dev/null)
    [[ -n $compile_elapsed ]] || compile_elapsed=0
    print -- "----- stage7 compile stdout/stderr begin -----"
    cat "$compile_output"
    print -- "----- stage7 compile stdout/stderr end -----"
    print "STAGE7_META stage7_compile_output=$compile_output"
    print "STAGE7_META stage7_compile_rc=$compile_rc"
    print "STAGE7_META stage7_compile_end_uptime_s=$t1"
    print "STAGE7_META stage7_compile_elapsed_s=$compile_elapsed"

    /usr/bin/awk "BEGIN{exit !((\"$t1\"+0) >= (\"$t0\"+0))}" || {
        print "STAGE7_META stage7_uptime_monotonic=no"
        return 1
    }
    print "STAGE7_META stage7_uptime_monotonic=yes"

    artifact=$(find "$worktree/target/$axtgt" -type f \
        \( -name arceos-helloworld -o -name helloworld \) \
        2>/dev/null | head -n 1)
    if [[ -n $artifact && -f $artifact ]]; then
        bytes=$(/usr/bin/wc -c <"$artifact") || return
        artifact_sha=$(sha256sum "$artifact" | /usr/bin/awk '{print $1}') || return
    fi
    print "STAGE7_META stage7_artifact_path=${artifact:-missing}"
    print "STAGE7_META stage7_artifact_bytes=$bytes"
    print "STAGE7_META stage7_artifact_sha256=$artifact_sha"

    if (( compile_rc != 0 || bytes < 500000 )); then
        print "BUILDSTORM_COMPILE mode=stage7 ok=false rc=$compile_rc elapsed_s=$compile_elapsed cores=$(/usr/bin/nproc) jobs=$jobs_label bytes=$bytes arch=$axarch"
        return 1
    fi
    sync || return
    print "BUILDSTORM_COMPILE mode=stage7 ok=true elapsed_s=$compile_elapsed cores=$(/usr/bin/nproc) jobs=$jobs_label bytes=$bytes arch=$axarch"
    print "BUILDSTORM_STAGE7_COMPILE ok"
}

probe_stage9_feedback() {
    print "STAGE9_META stage9_case=stage9-perf-feedback"
    print "STAGE9_META stage9_guest_nproc=$(/usr/bin/nproc)"
    print "STAGE9_META stage9_payload=/a20-probe/stage9-perf-probe"
    /a20-probe/stage9-perf-probe
}

run_named_case() {
    case "$1" in
    static-elf) probe_static_elf ;;
    cwd-root) probe_cwd_root ;;
    cwd-glibc) probe_cwd_glibc ;;
    cwd-deep) probe_cwd_deep ;;
    rustup-proxy-rustc) probe_proxy_rustc ;;
    rustup-proxy-cargo) probe_proxy_cargo ;;
    real-toolchain-rustc-bare) probe_real_rustc_bare ;;
    real-toolchain-rustc-with-lib) probe_real_rustc ;;
    real-toolchain-cargo) probe_real_cargo ;;
    directory-immediate-visibility) probe_directory_visibility ;;
    stage2-directory-100) probe_stage2_directory_100 ;;
    stage2-rustup-rustc-100) probe_stage2_proxy_rustc_100 ;;
    stage2-rustup-cargo-100) probe_stage2_proxy_cargo_100 ;;
    dynamic-pie-dso-pages) probe_dynamic_pages ;;
    stage3-dynamic-pie-dso-100) probe_stage3_dynamic_pages_100 ;;
    direct-rustc-hello) probe_rustc_hello ;;
    cargo-minibuild-j1) probe_cargo_minibuild_j1 ;;
    cargo-minibuild-default) probe_cargo_minibuild_default ;;
    stage4-cargo-minibuild) probe_stage4_cargo_minibuild ;;
    stage5-official-minibuild) probe_stage5_official_minibuild ;;
    stage6-ext4-dir-tail) probe_stage6_ext4_dir_tail ;;
    stage6-precompiled-helper) probe_stage6_precompiled_helper ;;
    stage7-shebang-exec) probe_stage7_shebang_exec ;;
    stage7-rustc-j8) probe_stage7_parallel_rustc ;;
    stage7-rustc-llvm-j8) probe_stage7_parallel_llvm ;;
    stage9-perf-feedback) probe_stage9_feedback ;;
    stage7-full-j1|stage7-full-j2|stage7-full-j4|stage7-full-j8|stage7-full-default)
        probe_stage7_full_build "$1"
        ;;
    *)
        print "[BUILDSTORM-PROBE][FATAL] unknown case: $1"
        return 2
        ;;
    esac
}

if [[ ${1:-} == --case ]]; then
    [[ -n ${2:-} ]] || exit 2
    run_named_case "$2"
    exit $?
fi
if [[ ${1:-} == --only ]]; then
    [[ -n ${2:-} ]] || exit 2
    print "#### A20OS BUILDSTORM SELECTED PROBE START arch=$arch ####"
    run_case "$2"
    print "[BUILDSTORM-PROBE] summary total=$total failures=$failures"
    print "#### A20OS BUILDSTORM SELECTED PROBE END arch=$arch ####"
    exit 0
fi

print "#### A20OS BUILDSTORM STAGE3 PROBE START arch=$arch ####"
run_case static-elf
run_case cwd-root
run_case cwd-glibc
run_case cwd-deep
run_case rustup-proxy-rustc
run_case rustup-proxy-cargo
run_case real-toolchain-rustc-bare
run_case real-toolchain-rustc-with-lib
run_case real-toolchain-cargo
run_case directory-immediate-visibility
run_case stage2-directory-100
run_case stage2-rustup-rustc-100
run_case stage2-rustup-cargo-100
run_case dynamic-pie-dso-pages
run_case stage3-dynamic-pie-dso-100
run_case direct-rustc-hello
run_case cargo-minibuild-j1
run_case cargo-minibuild-default
print "[BUILDSTORM-PROBE] summary total=$total failures=$failures"
print "#### A20OS BUILDSTORM STAGE3 PROBE END arch=$arch ####"

# Probe failures are diagnostic outcomes, not a runner failure.  Every case
# emits its own return code, and the host archives the complete serial stream.
exit 0
