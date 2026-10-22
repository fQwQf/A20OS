#!/bin/sh
# One private ext4 directory; the caller supplies the identical APK sysbench
# command (musl loader + library path on the Linux host). Never removes a
# pre-existing directory. Preparation, full read verification and cleanup are
# outside the FILEIO_RUN markers used for throughput parsing.
set -u
workdir=$1
busybox=$2
fsync_freq=$3
shift 3
mkdir "$workdir" || exit 1
fs_type=$("$busybox" stat -f -c %t "$workdir") || {
    rmdir "$workdir"
    exit 1
}
case "$fs_type" in ef53|EF53) ;; *)
    echo "FILEIO: ext4 required, found $fs_type" >&2
    rmdir "$workdir"
    exit 1
    ;;
esac
set -- "$@" fileio --threads=1 --file-num=1 --file-total-size=64M \
    --file-block-size=4096 --file-io-mode=sync --validate=on \
    --file-fsync-freq="$fsync_freq" --file-fsync-end=on --rand-seed=271828
(
    cd "$workdir" || exit 1
    echo "FILEIO_PREPARE fs_type=$fs_type bytes=67108864 fsync_freq=$fsync_freq"
    "$@" --file-test-mode=rndwr prepare || exit 1
    "$busybox" sync test_file.0 || exit 1
    echo FILEIO_RUN_BEGIN
    "$@" --file-test-mode=rndwr --time=5 --events=0 run || exit 1
    echo FILEIO_RUN_END
    echo FILEIO_VERIFY_BEGIN
    "$@" --file-test-mode=seqrd --file-fsync-freq=0 --file-fsync-end=off \
        --time=0 --events=16384 run || exit 1
    echo FILEIO_VERIFY_END
)
result=$?
(
    cd "$workdir" || exit 1
    "$@" --file-test-mode=rndwr cleanup
) || result=1
rmdir "$workdir" || result=1
if [ "$result" -eq 0 ]; then
    echo 'FILEIO: PASS verified_bytes=67108864 cleaned=1'
fi
exit "$result"
