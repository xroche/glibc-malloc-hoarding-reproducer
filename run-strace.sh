#!/bin/bash
# Capture syscall counts for the reproducer under system and patched
# glibc.  madvise/brk/mmap/munmap are the RSS-relevant syscalls.
#
# Usage:
#   ./run-strace.sh                       # system glibc only
#   ./run-strace.sh --patched DIR         # add patched contrast
#
# Requires strace (sudo apt install strace) or equivalent.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/reproducer-mtrace.c"
BIN="$HERE/reproducer-mtrace"
PATCHED_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --patched) PATCHED_DIR="${2:-}"; shift 2 ;;
        --help|-h)
            sed -n '3,11p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v strace >/dev/null || {
    echo "ERROR: strace not installed (sudo apt install strace)" >&2
    exit 1
}

if [ ! -f "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    gcc -O2 -g -o "$BIN" "$SRC"
fi

run_it () {
    local label="$1"; shift
    echo ""
    echo "============================================================"
    echo "== strace -c ($label)"
    echo "============================================================"
    # -c summary, -e trace= filters, -w wall-clock
    # Redirect program stdout to /dev/null so the syscall table lands
    # on strace's stderr unmolested.
    strace -c -e trace=madvise,brk,mmap,munmap -w \
        "$@" "$BIN" >/dev/null
}

run_it "system glibc"

if [ -n "$PATCHED_DIR" ]; then
    loader=$(ls "$PATCHED_DIR"/elf/ld-linux*.so* 2>/dev/null | head -1 || true)
    [ -n "$loader" ] || { echo "ERROR: $PATCHED_DIR/elf/ld-linux*.so* not found" >&2; exit 1; }
    run_it "patched glibc" "$loader" --library-path "$PATCHED_DIR"
fi
