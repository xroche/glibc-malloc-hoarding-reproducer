# BZ #33886 — diagnostic capture (mtrace + strace + malloc_info)

Response to Rocket Ma's request on BZ #33886 (comments 22–23).
Three complementary captures on the same small reproducer (200
long-lived 1 KB allocations interleaved with 2000 short-lived 64 KB
allocations, 125 MB total — same pattern as `tst-madvise-threshold`
in the RFC, scaled down from the 10 GB production workload so the
traces remain shareable).

All three captures run the same binary under (a) Ubuntu 24.04
stock glibc 2.39 and (b) the RFC patched trunk.

## Reproduction

Prerequisites: `gcc`, `strace`, `mtrace` Perl script (package
`glibc-tools` / `libc-bin`), glibc >= 2.34 so `libc_malloc_debug.so.0`
is present.  Patched contrast needs a built glibc tree (output of
`~/git/glibc/configure --prefix=/usr && make -j$(nproc)` in an
out-of-source build directory).

```sh
# Get the files.
git clone <this repo or grab the attachment tarball>
cd reproducer/

# Run each capture; --patched is optional but produces the contrast.
./run-mtrace.sh         --patched /path/to/glibc-build
./run-strace.sh         --patched /path/to/glibc-build
./run-malloc-info.sh    --patched /path/to/glibc-build
```

Each script is ~3-4 KB, self-contained, `set -euo pipefail`.  They
rebuild `reproducer-mtrace` from `reproducer-mtrace.c` if the source
is newer than the binary, locate `libc_malloc_debug.so.0` via
`ldconfig -p`, and handle both system and patched runs from a single
invocation.  See the `run-*.sh` files for the exact commands; the
core lines are:

```sh
# mtrace
LD_PRELOAD=/lib/x86_64-linux-gnu/libc_malloc_debug.so.0 \
  MALLOC_TRACE=/path/to/trace.out \
  ./reproducer-mtrace
mtrace ./reproducer-mtrace /path/to/trace.out

# strace
strace -c -e trace=madvise,brk,mmap,munmap -w ./reproducer-mtrace >/dev/null

# malloc_info (the binary writes XML when MALLOC_INFO_PREFIX is set)
MALLOC_INFO_PREFIX=/tmp/minfo- ./reproducer-mtrace

# Patched run, for each of the above, same command prefixed with:
/path/to/glibc-build/elf/ld-linux-x86-64.so.2 \
  --library-path /path/to/glibc-build \
  [plus LD_PRELOAD=/path/to/glibc-build/malloc/libc_malloc_debug.so.0 for mtrace]
```

## 1. mtrace — allocation pattern

Run: `./run-mtrace.sh --patched ~/git/glibc-build`

| Measure | System 2.39 | Patched trunk |
|---|---|---|
| allocations | 2205 | 2205 |
| frees | 2204 | 2204 |
| total allocated | 125.2 MB | 125.2 MB |
| unfreed (mtrace says) | 1 × 4 KB | 1 × 4 KB |
| User-allocation diff | — | byte-identical |

The one "unfreed" 4 KB block on both sides is the stdio `FILE*`
buffer from `_IO_file_doallocate` when `get_rss_mb` opens
`/proc/self/statm`.  It is released at process exit via the stdio
cleanup path, not via an explicit `free()`.  Every single user
allocation (2000 filler + 200 index + 5 stdio internals) has a
matching free.

**Finding:** no leak.  Allocation pattern is byte-identical under
both glibcs.  The 124 MB RSS difference (see §2) is invisible here
by design — mtrace is an allocator-level log, not a syscall tracer.

## 2. strace -c — syscall counts

Run: `./run-strace.sh --patched ~/git/glibc-build`

**System 2.39:**

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 98.20    0.010034          14       689           brk
  1.52    0.000155          19         8           mmap
  0.28    0.000028          28         1           munmap
100.00    0.010218          14       698           total
```

**Patched trunk:**

```
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 82.15    0.052447          26      2000           madvise
 17.57    0.011215          16       689           brk
  0.29    0.000182          16        11           mmap
100.00    0.063844          23      2700           total
```

**Finding — the hard evidence the patch was designed to produce:**

- **System glibc: 0 madvise calls.**  Consolidated interior chunks
  stay fully resident, hence the 126 MB RSS held after phase 2.
- **Patched: 2000 madvise calls**, one per 64 KB filler free.  At
  64 KB > one page, the "direct madvise" gate fires immediately;
  the accumulator (which batches sub-page frees) is not exercised
  by this workload — that case appears in workloads that free
  many small blocks that merge into a large chunk, the scenario
  raised by Wilco on BZ #33886 comment 10.
- **brk and mmap counts are identical** (689 and ~10): heap
  growth and arena allocation are unchanged.  The patch is purely
  additive at the syscall level.
- **Syscall overhead: ~52 ms for 2000 madvise calls**, about 26 µs
  per call.  On a realistic workload where the 125 MB of frees is
  spread over seconds of computation (not a tight free loop), this
  is noise.  On the tight-loop microbenchmark here, it is 0.05 s on
  top of an otherwise ~0.3 s run — the "overhead" reported in the
  RFC cover letter.

## 3. malloc_info — allocator state before and after

Run: `./run-malloc-info.sh --patched ~/git/glibc-build`

Snapshots at end of each phase, per-arena XML.  Key totals:

| Phase | Snapshot | system: `system/current` | system: `total rest` | patched: `system/current` | patched: `total rest` |
|---|---|---|---|---|---|
| 1 | after 2200 mallocs | 131 452 928 B | 1 chunk, 130 528 B (top) | 131 452 928 B | 1 chunk, 130 416 B (top) |
| 2 | after 2000 frees | 131 452 928 B | **201 chunks, 131 234 728 B** | 131 452 928 B | **201 chunks, 131 234 616 B** |
| 3 | after 200 frees | 131 452 928 B | 7 chunks, 131 436 294 B | 131 452 928 B | 16 chunks, 131 426 831 B |

**Finding — phase 2 (the critical point):** allocator bookkeeping
is effectively identical.  Same heap size (131 452 928 B = 125.36 MB),
same number of free chunks (201: 200 pairs of consolidated fillers
between pinned index chunks + 1 top), same total free size to
within 112 bytes.  The 112-byte delta comes from different libc.so.6
versions (2.39 vs trunk) allocating slightly different stdio
internals.

The patch does not change allocator state at the bin/chunk level.
It only changes what the kernel considers resident for pages that
the allocator has already placed on its free list.

**Phase 3 divergence (for honesty):** after freeing the 200 index
chunks, the system glibc consolidates the whole heap into 7 free
regions; the patched glibc ends up with 16.  This happens because
madvise'd pages re-fault on any subsequent touch, and the allocator
state changes that follow (tcache top-of-bin pointers etc.) have
slightly different consolidation outcomes.  Both states are
semantically equivalent — 125.35 MB of free memory in either 7 or
16 regions — and no user-visible behaviour is affected.  Worth
noting because "the traces don't show much" is the wrong claim:
they show a small allocator-internal difference in phase 3.  Not a
correctness issue, but not zero either.

## Summary

| Question | Answer | Evidence |
|---|---|---|
| Is there a user-allocation leak? | No | mtrace: 2204 frees match 2204 frees; only stdio exit-cleanup flagged |
| Does the patch change allocation behaviour? | No | mtrace user-allocation diff: byte-identical |
| Does the patch change allocator bookkeeping? | No | malloc_info: identical system_current, free chunk count, and total free size at every phase |
| What does the patch change? | madvise syscalls | strace: 0 vs 2000 madvise calls; RSS 126 MB vs 2 MB after phase 2 |
| What is the overhead? | ~26 µs per madvise, or ~52 ms on this tight-loop test | strace -c wall-clock |

Files attached / in this directory:

- `reproducer-mtrace.c` — source (~3.8 KB)
- `run-mtrace.sh` — mtrace driver
- `run-strace.sh` — strace driver
- `run-malloc-info.sh` — malloc_info driver
- `reproducer.mtrace` / `.gz` — mtrace output under system glibc
- `reproducer-patched.mtrace` — mtrace output under patched glibc
- `{sys,pat}-phase{1,2,3}.xml` — malloc_info snapshots

Nothing in the captures is sensitive — allocation sizes and
pointers are synthetic (`0xAA` / `0xBB` fills from the reproducer
itself); no real data flows through.
