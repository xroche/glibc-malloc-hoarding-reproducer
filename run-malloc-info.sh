#!/bin/bash
# Capture malloc_info(0, fp) snapshots at each reproducer phase, under
# system and (optionally) patched glibc.  Compares per-phase
# allocator state.
#
# Usage:
#   ./run-malloc-info.sh                     # system glibc
#   ./run-malloc-info.sh --patched DIR       # add patched contrast

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/reproducer-mtrace.c"
BIN="$HERE/reproducer-mtrace"
OUT_DIR="${OUT_DIR:-$HERE}"
PATCHED_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --patched) PATCHED_DIR="${2:-}"; shift 2 ;;
        --help|-h)
            sed -n '3,9p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    gcc -O2 -g -o "$BIN" "$SRC"
fi

run_it () {
    local label="$1"; local prefix="$2"; shift 2
    echo ""
    echo "== malloc_info snapshots ($label)"
    MALLOC_INFO_PREFIX="$prefix" "$@" "$BIN" | sed 's/^/  /'
}

run_it "system glibc" "$OUT_DIR/sys-" env

if [ -n "$PATCHED_DIR" ]; then
    loader=$(ls "$PATCHED_DIR"/elf/ld-linux*.so* 2>/dev/null | head -1 || true)
    [ -n "$loader" ] || { echo "ERROR: $PATCHED_DIR/elf/ld-linux*.so* not found" >&2; exit 1; }
    run_it "patched glibc" "$OUT_DIR/pat-" \
        env "$loader" --library-path "$PATCHED_DIR"
fi

echo ""
echo "== key totals per phase"
for phase in phase1 phase2 phase3; do
    for prefix in sys pat; do
        f="$OUT_DIR/${prefix}-${phase}.xml"
        [ -f "$f" ] || continue
        sys_cur=$(grep -m1 '<system type="current"' "$f" | grep -oE 'size="[0-9]+"' | head -1)
        rest=$(grep '<total type="rest"' "$f" | head -1)
        printf '  %-10s %-4s %s  %s\n' "$phase" "$prefix" "$sys_cur" "$rest"
    done
done
