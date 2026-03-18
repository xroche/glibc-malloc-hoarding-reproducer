# Tentative patches

Two patch variants against glibc trunk (2.43.9000, commit dd5ebf3ed8).
Both are `git apply`-ready.

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

## Patch 2: default-on in _int_free_maybe_trim (regression fix)

`0002-malloc-madvise-interior-chunks-default-on.patch`

Adds the madvise call in `_int_free_maybe_trim`, gated by the existing
`ATTEMPT_TRIMMING_THRESHOLD` (64 KB). No tunable, no configuration. Always on
for consolidated chunks >= 64 KB.

Framed as a regression fix: restores the memory-return behavior from before
glibc 2.26. The code path already calls `systrim` and `heap_trim` (both
syscalls), so one more `madvise` is the same class of operation.

Changes: `malloc/malloc.c` only.

## Results with the reproducer

16 threads, 256 MB live data, 10 GB query throughput.

### Patch 1 (tunable, threshold=64K)

```
                      Baseline    Patched
RSS after free():     1247 MB     296 MB
Live data:            261 MB      261 MB
malloc_trim recovery: 962 MB      14 MB
Runtime:              0.37s       0.50s
When disabled:        -           identical to baseline
```

### Patch 2 (default-on)

```
                      Baseline    Patched
RSS after free():     1247 MB     296 MB
Live data:            261 MB      261 MB
malloc_trim recovery: 962 MB      14 MB
Runtime:              0.37s       0.52s
```

Both patches produce the same RSS reduction. Patch 2 is slightly slower
because it always runs (no threshold check to skip small chunks).

## Apply and build

```sh
cd /path/to/glibc

# Patch 1 (tunable):
git apply patch/0001-malloc-add-madvise_threshold-tunable.patch

# OR Patch 2 (default-on):
git apply patch/0002-malloc-madvise-interior-chunks-default-on.patch

# Build:
mkdir -p ../glibc-build && cd ../glibc-build
../glibc/configure --prefix=/usr
make -j$(nproc)

# Test (patch 1 only, patch 2 has no separate test):
make test t=malloc/tst-madvise-threshold
```

## AI disclosure

Both patches were written with AI assistance (Claude). The author reviewed,
tested, and validated all code. The patch logic reuses the existing `mtrim()`
page-alignment pattern.
