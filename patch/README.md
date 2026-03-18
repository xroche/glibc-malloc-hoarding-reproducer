# Tentative patches

Three patch variants against glibc trunk (2.43.9000, commit dd5ebf3ed8).
All are `git apply`-ready.

## Regression context

Bisect shows the memory hoarding regression was introduced in **glibc 2.26**
(tcache). Before tcache, fastbin consolidation created free regions large enough
to trigger `systrim`/`heap_trim`. tcache bypasses consolidation, so freed
chunks never merge past the trimming threshold.

## Patch 1: opt-in tunable (conservative)

`0001-malloc-add-madvise_threshold-tunable.patch`

Adds `glibc.malloc.madvise_threshold`. When set to a positive value, `free()`
calls `madvise(MADV_DONTNEED)` on the page-aligned interior of consolidated
free chunks above the threshold. Disabled by default.

Changes: `elf/dl-tunables.list`, `malloc/malloc.c`, `malloc/arena.c`,
`malloc/Makefile`, `malloc/tst-madvise-threshold.c` (new test).

## Patch 2: default-on, madvise every large chunk

`0002-malloc-madvise-interior-chunks-default-on.patch`

Adds the madvise call in `_int_free_maybe_trim`, gated by the existing
`ATTEMPT_TRIMMING_THRESHOLD` (64 KB). No tunable, no configuration.

Best RSS recovery, but triggers a madvise syscall on every large consolidated
free. Freeing many adjacent small blocks that merge progressively causes
excessive madvise calls (see benchmark below).

Changes: `malloc/malloc.c` only.

## Patch 3: default-on, batched via per-arena accumulator (recommended)

`0003-malloc-madvise-with-accumulator.patch`

Tracks cumulative freed bytes (before consolidation) per arena. Only fires
madvise when the accumulator crosses 128 KB AND the consolidated chunk is
>= 64 KB. Resets the accumulator after each madvise.

Accumulating the original freed size (not the merged result) is what prevents
the madvise storm: a million 100-byte frees grow the accumulator at 100 bytes
per call, not at the progressively larger consolidated size.

Changes: `malloc/malloc.c` only.

## Results with the reproducer

16 threads, 256 MB live data, 10 GB query throughput.

```
                      Baseline    Patch 1     Patch 2     Patch 3
                                  (tunable)   (every)     (batched)
RSS after free():     1247 MB     296 MB      296 MB      707 MB
Live data:            261 MB      261 MB      261 MB      261 MB
malloc_trim recovery: 962 MB      14 MB       14 MB       424 MB
Runtime:              0.37s       0.50s       0.52s       0.47s
```

## Adversarial workloads

Sequential free of adjacent blocks (Wilco's concern, BZ #33886 comment 10):

```
                      Baseline    Patch 2     Patch 3
1M x 100B free:       10 ms       TBD         30 ms       (0 madvise calls)
100K x 8KB free:      7.8 ms      177 ms      72 ms
  madvise calls:      0           99,603      6,250
```

Patch 3 reduces madvise calls by 16x vs patch 2 on the 8 KB sequential case.

## Apply and build

```sh
cd /path/to/glibc

# Pick one:
git apply patch/0001-malloc-add-madvise_threshold-tunable.patch
git apply patch/0002-malloc-madvise-interior-chunks-default-on.patch
git apply patch/0003-malloc-madvise-with-accumulator.patch

# Build:
mkdir -p ../glibc-build && cd ../glibc-build
../glibc/configure --prefix=/usr
make -j$(nproc)

# Test (patch 1 only):
make test t=malloc/tst-madvise-threshold
```

## AI disclosure

All patches were written with AI assistance (Claude). The author reviewed,
tested, and validated all code. The patch logic reuses the existing `mtrim()`
page-alignment pattern.
