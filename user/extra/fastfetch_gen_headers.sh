#!/bin/sh
# fastfetch_gen_headers.sh — Generate config headers for fastfetch without CMake
#
# Usage: fastfetch_gen_headers.sh <fastfetch_src_dir> <output_dir>
#
# Generates:
#   <output_dir>/fastfetch_config.h
#   <output_dir>/fastfetch_datatext.h
#   <output_dir>/logo_builtin.h

set -e

FF_SRC="$1"
OUT_DIR="$2"

if [ -z "$FF_SRC" ] || [ -z "$OUT_DIR" ]; then
    echo "Usage: $0 <fastfetch_src_dir> <output_dir>" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# ---------------------------------------------------------------
# fastfetch_config.h
# ---------------------------------------------------------------
cat > "$OUT_DIR/fastfetch_config.h" <<'HCONFIG'
#pragma once

#define FASTFETCH_PROJECT_NAME "fastfetch"
#define FASTFETCH_PROJECT_VERSION "2.66.0"
#define FASTFETCH_PROJECT_VERSION_MAJOR 2
#define FASTFETCH_PROJECT_VERSION_MINOR 66
#define FASTFETCH_PROJECT_VERSION_PATCH 0
#define FASTFETCH_PROJECT_VERSION_GIT ""
#define FASTFETCH_PROJECT_VERSION_TWEAK ""
#define FASTFETCH_PROJECT_VERSION_TWEAK_NUM 0
#define FASTFETCH_PROJECT_CMAKE_BUILD_TYPE "Release"
#define FASTFETCH_PROJECT_HOMEPAGE_URL "https://github.com/fastfetch-cli/fastfetch"
#define FASTFETCH_PROJECT_DESCRIPTION "Fast neofetch-like system information tool"
#define FASTFETCH_PROJECT_LICENSE "MIT license"

#define FASTFETCH_TARGET_DIR_ROOT ""
#define FASTFETCH_TARGET_DIR_USR "/usr"
#define FASTFETCH_TARGET_DIR_HOME "/home"
#define FASTFETCH_TARGET_DIR_ETC "/etc"

#define FASTFETCH_TARGET_DIR_INSTALL_SYSCONF "/etc"
HCONFIG

echo "[FASTFETCH] Generated fastfetch_config.h"

# ---------------------------------------------------------------
# fastfetch_datatext.h  (embed help.json + default module structure)
# ---------------------------------------------------------------

# Helper: encode file content as a C string literal
encode_c_string() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c '
import sys
data = open(sys.argv[1], "r", encoding="utf-8", errors="replace").read()
data = data.rstrip("\n")
data = data.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n")
sys.stdout.write("\"" + data + "\"")
' "$1"
    else
        awk '
        BEGIN { ORS="" }
        NR > 1 { printf "\\n" }
        {
            gsub(/\\/, "\\\\")
            gsub(/"/, "\\\"")
            print
        }
        ' "$1" | sed 's/^/"/; s/$/"/'
    fi
}

DATATEXT_JSON_HELP=$(encode_c_string "$FF_SRC/doc/help.json")
DATATEXT_STRUCTURE='"Title:Separator:OS:Host:Kernel:Uptime:Packages:Shell:Display:DE:WM:WMTheme:Theme:Icons:Font:Cursor:Terminal:TerminalFont:CPU:GPU:Memory:Swap:Disk:LocalIp:Battery:PowerAdapter:Locale:Break:Colors"'

cat > "$OUT_DIR/fastfetch_datatext.h" <<DATACFG
#pragma once

#define FASTFETCH_DATATEXT_JSON_HELP ${DATATEXT_JSON_HELP}
#define FASTFETCH_DATATEXT_STRUCTURE ${DATATEXT_STRUCTURE}
DATACFG

echo "[FASTFETCH] Generated fastfetch_datatext.h"

# ---------------------------------------------------------------
# logo_builtin.h  (embed all ASCII logo files)
# ---------------------------------------------------------------

{
    printf '%s\n' '#pragma once'
    printf '%s\n' '#pragma GCC diagnostic ignored "-Wtrigraphs"'
    printf '%s\n' ''
    find "$FF_SRC/src/logo/ascii" -mindepth 2 -maxdepth 2 -name '*.txt' -type f | sort | while IFS= read -r logo_file; do
        name=$(basename "$logo_file" .txt)
        upper=$(echo "$name" | tr '[:lower:]' '[:upper:]')
        content=$(encode_c_string "$logo_file")
        printf '#define FASTFETCH_DATATEXT_LOGO_%s %s\n' "$upper" "$content"
    done
} > "$OUT_DIR/logo_builtin.h"

echo "[FASTFETCH] Generated logo_builtin.h"
