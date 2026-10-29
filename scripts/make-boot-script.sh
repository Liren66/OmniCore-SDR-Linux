#!/usr/bin/env bash
# Generate both the readable boot script and the U-Boot image from one banner.
# Usage: bash scripts/make-boot-script.sh <output-directory>
set -euo pipefail
W=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
mkdir -p "$OUT"
{
    while IFS= read -r line; do
        # Single quotes protect the ASCII art's backslashes and backtick.
        # Escape any apostrophe as a quoted literal between quoted strings.
        line=${line//\'/\'\\\'\'}
        printf "echo '%s'\n" "$line"
    done < "$W/configs/banner.txt"
    cat "$W/configs/boot.cmd"
} > "$OUT/boot.cmd"
mkimage -A arm -T script -C none -n "OmniCore boot" \
    -d "$OUT/boot.cmd" "$OUT/boot.scr"
