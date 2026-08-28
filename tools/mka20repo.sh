#!/bin/sh
# mka20repo.sh — index a directory of .apk packages into an apk repository.
#
# Usage:
#   tools/mka20repo.sh [options] REPO_ARCH_DIR
#
#   REPO_ARCH_DIR    directory containing *.apk (e.g. build/repo/riscv64);
#                    APKINDEX.tar.gz is written into it
#
# Options:
#   --sign-key KEY   RSA private key (PEM); signs APKINDEX.tar.gz
#   --key-name NAME  public key file name (default: <key basename>.pub)
#   --keys-dir DIR   trust directory for verifying package signatures while
#                    indexing (default: directory of --sign-key)
#
# Without --sign-key the index is left unsigned and packages are accepted
# untrusted (compose with `mkrootfs.py --allow-untrusted`).
#
# Env: APK_STATIC — path to a static apk binary (default: auto-fetch via
# tools/ensure-apk-static.sh).

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

sign_key=
key_name=
keys_dir=
repo_dir=

while [ $# -gt 0 ]; do
    case "$1" in
        --sign-key) sign_key=$2; shift 2 ;;
        --key-name) key_name=$2; shift 2 ;;
        --keys-dir) keys_dir=$2; shift 2 ;;
        -*) echo "mka20repo: unknown option $1" >&2; exit 2 ;;
        *)
            if [ -n "$repo_dir" ]; then
                echo "mka20repo: exactly one REPO_ARCH_DIR expected" >&2; exit 2
            fi
            repo_dir=$1; shift ;;
    esac
done

[ -n "$repo_dir" ] || { echo "usage: mka20repo.sh [--sign-key KEY [--key-name NAME] [--keys-dir DIR]] REPO_ARCH_DIR" >&2; exit 2; }
[ -d "$repo_dir" ] || { echo "mka20repo: $repo_dir is not a directory" >&2; exit 1; }

apk_static=${APK_STATIC:-$("$script_dir/ensure-apk-static.sh")}

if [ -n "$sign_key" ]; then
    [ -f "$sign_key" ] || { echo "mka20repo: sign key $sign_key not found" >&2; exit 1; }
    sign_key=$(CDPATH= cd -- "$(dirname -- "$sign_key")" && pwd)/$(basename -- "$sign_key")
    keys_dir=${keys_dir:-$(dirname "$sign_key")}
    keys_dir=$(CDPATH= cd -- "$keys_dir" && pwd)
    trust_args="--keys-dir $keys_dir"
else
    trust_args="-U --allow-untrusted"
fi

cd "$repo_dir"
set -- ./*.apk
[ -f "$1" ] || { echo "mka20repo: no .apk files in $repo_dir" >&2; exit 1; }

rm -f APKINDEX.tar.gz.unsigned
# shellcheck disable=SC2086
if ! "$apk_static" $trust_args index -o APKINDEX.tar.gz.unsigned "$@"; then
    echo "mka20repo: apk index failed" >&2
    exit 1
fi
mv APKINDEX.tar.gz.unsigned APKINDEX.tar.gz

if [ -n "$sign_key" ]; then
    if [ -n "$key_name" ]; then
        python3 "$script_dir/apk-sign-stream.py" --key "$sign_key" \
            --key-name "$key_name" APKINDEX.tar.gz
    else
        python3 "$script_dir/apk-sign-stream.py" --key "$sign_key" APKINDEX.tar.gz
    fi
else
    echo "mka20repo: unsigned index written (compose with --allow-untrusted)"
fi
