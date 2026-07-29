#!/bin/mksh
#
# BuildStorm stage-1 diagnostic probe.
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
    stage2-*-100|stage3-*-100|stage4-*|stage5-*) case_timeout=900 ;;
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
