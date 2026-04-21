#!/bin/bash
# Reproduce the mtrace capture for BZ #33886.
#
# Builds reproducer-mtrace.c, runs it under LD_PRELOAD=libc_malloc_debug.so.0
# with MALLOC_TRACE pointing at a trace file, and summarises the result.
# Optionally also runs against a patched glibc build tree for contrast.
#
# Usage:
#   ./run-mtrace.sh                    # run against system glibc
#   ./run-mtrace.sh --patched DIR      # also run against patched build in DIR
#                                      # (DIR must contain libc.so.6, ld-linux*.so*,
#                                      #  and malloc/libc_malloc_debug.so.0)
#   ./run-mtrace.sh --help
#
# Prereqs: gcc, glibc >= 2.34 (has libc_malloc_debug.so split),
#          mtrace Perl script (from package glibc or libc6-dev).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/reproducer-mtrace.c"
BIN="$HERE/reproducer-mtrace"
OUT_DIR="${OUT_DIR:-$HERE}"
PATCHED_DIR=""

usage () {
    sed -n '3,18p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --patched)
            PATCHED_DIR="${2:-}"
            [ -n "$PATCHED_DIR" ] || { echo "--patched needs a directory" >&2; exit 2; }
            shift 2 ;;
        --help|-h) usage 0 ;;
        *)  echo "unknown arg: $1" >&2; usage 2 ;;
    esac
done

# 1) Locate the system libc_malloc_debug.so.0.
find_sys_debug_lib () {
    # Ask the runtime linker what name resolves to.
    ld_path=$(ldconfig -p 2>/dev/null | awk '
        $1 == "libc_malloc_debug.so.0" && / \(libc6,x86-64\) / { print $NF; exit }
        $1 == "libc_malloc_debug.so.0" && !/ \(libc6,/         { print $NF; exit }
    ')
    if [ -n "$ld_path" ] && [ -e "$ld_path" ]; then
        echo "$ld_path"
        return 0
    fi
    for cand in /usr/lib/x86_64-linux-gnu/libc_malloc_debug.so.0 \
                /usr/lib/aarch64-linux-gnu/libc_malloc_debug.so.0 \
                /usr/lib64/libc_malloc_debug.so.0 \
                /lib64/libc_malloc_debug.so.0 \
                /usr/lib/libc_malloc_debug.so.0; do
        [ -f "$cand" ] && { echo "$cand"; return 0; }
    done
    return 1
}

SYS_DEBUG_LIB=$(find_sys_debug_lib) || {
    echo "ERROR: libc_malloc_debug.so.0 not found on this system." >&2
    echo "  Install package 'libc6' (glibc >= 2.34) or equivalent." >&2
    exit 1
}
echo "System libc_malloc_debug: $SYS_DEBUG_LIB"

# 2) Build the reproducer (idempotent).
if [ ! -f "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    echo "Building $BIN ..."
    gcc -O2 -g -o "$BIN" "$SRC"
fi

# 3) Helper: run + analyse.
run_and_analyse () {
    local label="$1"
    local trace_file="$2"
    shift 2
    local runner=("$@")   # prefix command words, e.g. ld-linux loader + --library-path

    echo ""
    echo "============================================================"
    echo "== Running ($label)"
    echo "============================================================"

    rm -f "$trace_file"
    MALLOC_TRACE="$trace_file" "${runner[@]}" "$BIN"

    echo ""
    echo "-- trace stats --"
    wc -l "$trace_file"
    awk '
        /^@.*\+ / { allocs++; bytes += strtonum($NF) }
        /^@.*- /  { frees++ }
        END {
            printf "  allocations: %d\n  frees:       %d\n",
                   allocs, frees
            printf "  total allocated: %d bytes (%.1f MB)\n",
                   bytes, bytes / 1048576
        }' "$trace_file"

    echo ""
    echo "-- size distribution (top 5) --"
    awk '/^@.*\+ /{print $NF}' "$trace_file" | sort | uniq -c | sort -rn | head -5

    echo ""
    echo "-- mtrace leak check --"
    if command -v mtrace >/dev/null 2>&1; then
        # mtrace exits non-zero when it flags "Memory not freed"; don't let
        # set -e kill the script for that.
        mtrace "$BIN" "$trace_file" 2>&1 | head -20 || true
    else
        echo "  mtrace Perl script not installed — install package glibc-utils or libc-bin."
    fi
}

# 4) System run.
SYS_TRACE="$OUT_DIR/reproducer.mtrace"
run_and_analyse "system glibc" "$SYS_TRACE" \
    env LD_PRELOAD="$SYS_DEBUG_LIB"

# 5) Patched run (optional).
if [ -n "$PATCHED_DIR" ]; then
    loader=$(ls "$PATCHED_DIR"/elf/ld-linux*.so* 2>/dev/null | head -1 || true)
    patched_lib="$PATCHED_DIR/malloc/libc_malloc_debug.so.0"
    if [ -z "$loader" ] || [ ! -f "$patched_lib" ]; then
        echo ""
        echo "ERROR: --patched $PATCHED_DIR does not look like a glibc build tree."
        echo "       Expected elf/ld-linux*.so* and malloc/libc_malloc_debug.so.0."
        exit 1
    fi

    PATCHED_TRACE="$OUT_DIR/reproducer-patched.mtrace"
    run_and_analyse "patched glibc ($PATCHED_DIR)" "$PATCHED_TRACE" \
        env LD_PRELOAD="$patched_lib" \
            "$loader" --library-path "$PATCHED_DIR"

    echo ""
    echo "============================================================"
    echo "== Trace diff (user allocations only, heap addresses normalised)"
    echo "============================================================"
    # Keep only entries from the reproducer binary itself; drop stdio
    # internals from libc, which legitimately differ between the system
    # glibc and the patched tree (different versions of _IO_file_doallocate
    # etc.) and would obscure the comparison.  The caller field in the
    # trace is the absolute binary path.
    filter () {
        # shellcheck disable=SC2016
        awk -v bin="$BIN" '
            index($0, "@ " bin ":") == 1 {
                gsub(/0x[0-9a-f]+/, "0xADDR")
                print
            }' "$1"
    }
    filter "$SYS_TRACE"     > "$SYS_TRACE.norm"
    filter "$PATCHED_TRACE" > "$PATCHED_TRACE.norm"
    sys_lines=$(wc -l < "$SYS_TRACE.norm")
    pat_lines=$(wc -l < "$PATCHED_TRACE.norm")
    echo "  user-allocation lines: system=$sys_lines  patched=$pat_lines"
    if diff -q "$SYS_TRACE.norm" "$PATCHED_TRACE.norm" >/dev/null; then
        echo "  User allocations are byte-identical between the two runs."
        echo "  The RSS difference comes entirely from kernel-level madvise"
        echo "  activity added by the patch, which mtrace does not capture."
    else
        echo "  Traces differ; showing first 20 lines of diff:"
        diff "$SYS_TRACE.norm" "$PATCHED_TRACE.norm" | head -20
    fi
    rm -f "$SYS_TRACE.norm" "$PATCHED_TRACE.norm"
fi

echo ""
echo "Trace(s) saved to $OUT_DIR/reproducer{,-patched}.mtrace"
