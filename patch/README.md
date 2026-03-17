# Tentative patch

Patch against glibc trunk (2.43.9000, commit dd5ebf3ed8).

Adds a new tunable `glibc.malloc.madvise_threshold`. When set to a
positive value, `free()` calls `madvise(MADV_DONTNEED)` on the
page-aligned interior of consolidated free chunks larger than the
threshold. Disabled by default (threshold=0).

## Files changed

- `elf/dl-tunables.list` -- tunable declaration
- `malloc/malloc.c` -- field in `malloc_par`, setter, madvise logic in `_int_free_create_chunk`
- `malloc/arena.c` -- tunable callback registration
- `malloc/Makefile` -- test registration
- `malloc/tst-madvise-threshold.c` -- test case

## Results with the reproducer

```
                    Baseline    threshold=64K
RSS after free():   1247 MB     296 MB
Live data:          261 MB      261 MB
malloc_trim extra:  962 MB      14 MB
Runtime:            0.37s       0.50s
When disabled:      -           identical to baseline
```

## Apply and build

```sh
cd /path/to/glibc
git apply patch/0001-malloc-add-madvise_threshold-tunable.patch
mkdir -p ../glibc-build && cd ../glibc-build
../glibc/configure --prefix=/usr
make -j$(nproc)
make test t=malloc/tst-madvise-threshold
```

## AI disclosure

This patch was written with AI assistance (Claude). The author reviewed,
tested, and validated all code. The patch logic reuses the existing
`mtrim()` page-alignment pattern and is not novel.
