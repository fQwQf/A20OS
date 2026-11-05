#!/bin/sh
# Keep the historical entry point; Python checks ELF and Linux Image headers.
set -eu
exec "${PYTHON:-python3}" "$(dirname "$0")/kernel_bundle.py" "$@"
