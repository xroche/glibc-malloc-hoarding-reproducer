# Tentative patches

Two patch variants against glibc trunk (2.43.9000, commit dd5ebf3ed8).

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

Includes a test case (`malloc/tst-madvise-threshold.c`).

## Patch 2: default-on in _int_free_maybe_trim (bolder)

`0002-malloc-madvise-interior-chunks-default-on.patch`

Adds the madvise call directly in `_int_free_maybe_trim`, gated by the existing
`ATTEMPT_TRIMMING_THRESHOLD` (64 KB). No tunable, no configuration. Always on
for consolidated chunks >= 64 KB.

This is a regression fix: it restores the memory-return behavior that existed
before glibc 2.26. The code path already calls `systrim` and `heap_trim` (both
syscalls), so one more `madvise` is not a new class of overhead.

**Note:** Patch 2 is a conceptual diff showing the approach. It is not a
`git apply`-ready patch (line numbers are approximate). Patch 1 is the tested,
buildable version.

## Results with the reproducer

```
                    Baseline    With fix
RSS after free():   1247 MB     296 MB
Live data:          261 MB      261 MB
malloc_trim extra:  962 MB      14 MB
```

## Apply and build (patch 1)

```sh
cd /path/to/glibc
git apply patch/0001-malloc-add-madvise_threshold-tunable.patch
mkdir -p ../glibc-build && cd ../glibc-build
../glibc/configure --prefix=/usr
make -j$(nproc)
GLIBC_TUNABLES=glibc.malloc.madvise_threshold=65536 \
  make test t=malloc/tst-madvise-threshold
```

## AI disclosure

Both patches were written with AI assistance (Claude). The author reviewed,
tested, and validated all code. The patch logic reuses the existing `mtrim()`
page-alignment pattern.
