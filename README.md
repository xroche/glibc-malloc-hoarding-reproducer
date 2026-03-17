# glibc malloc memory hoarding reproducer

Demonstrates a problem in glibc's memory allocator: `free()` does not return
physical memory to the OS for interior free chunks. Long-running multi-threaded
programs can hold GB of freed memory that the kernel cannot reclaim.

Calling `malloc_trim(0)` recovers the memory, proving it was reclaimable all
along. Several projects (systemd, OpenJDK, Python) work around this by calling
`malloc_trim` periodically.

Related glibc bugs: [#15321](https://sourceware.org/bugzilla/show_bug.cgi?id=15321),
[#18910](https://sourceware.org/bugzilla/show_bug.cgi?id=18910),
[#27976](https://sourceware.org/bugzilla/show_bug.cgi?id=27976),
[#33886](https://sourceware.org/bugzilla/show_bug.cgi?id=33886).

Blog post with the full production story:
[When allocators are hoarding your precious memory](https://www.algolia.com/blog/engineering/when-allocators-are-hoarding-your-precious-memory)

## What the reproducer does

It simulates a server workload where long-lived data (an "index") is allocated
interleaved with short-lived data ("queries") across multiple threads. After all
query data is freed, the index data pins the arena heaps, and glibc holds on to
the freed query pages.

```
Heap layout after queries are freed:

  |--free--|--INDEX--|--free--|--INDEX--|--free--|--INDEX--|--small top--|
                                                           ^
                                    only this part can be trimmed by free()
```

## Quick start with Docker

### Prerequisites

Install Docker if you don't have it:

```sh
# Ubuntu/Debian
sudo apt-get update && sudo apt-get install -y docker.io
sudo usermod -aG docker $USER
# Log out and back in for the group change to take effect

# macOS
brew install --cask docker
# Then open Docker Desktop from Applications

# Verify it works
docker run --rm hello-world
```

### Build and run

```sh
git clone https://github.com/xroche/glibc-malloc-hoarding-reproducer.git
cd glibc-malloc-hoarding-reproducer

docker build -t malloc-repro .
docker run --rm malloc-repro
```

Expected output (numbers will vary):

```
=== glibc malloc hoarding reproducer ===
Threads: 16, Index: 16 MB/thread (256 MB total)
Bursts: 10 x 64 MB/thread (10240 MB total query data)
glibc version: 2.35

  Initial                              RSS:    1624 KB  (1 MB)
  Thread 0, burst 1/10                 RSS: 1063580 KB  (1038 MB)
  Thread 0, burst 10/10                RSS: 1243500 KB  (1214 MB)

Done in 0.34 seconds.  Index: 10037 chunks
  After bursts (queries freed)         RSS: 1277212 KB  (1247 MB)

malloc stats:
  arena (sbrk):   1256 MB
  in-use (sbrk):  261 MB
  free (sbrk):    995 MB  <-- hoarded, not returned to OS
  mmap'd:         0 MB

Calling malloc_trim(0)...
  malloc_trim took: 60.4 ms
  After malloc_trim                    RSS:  291708 KB  (284 MB)
  Recovered: 962 MB
```

The important lines:

- `in-use (sbrk): 261 MB` -- actual live data
- `free (sbrk): 995 MB` -- freed but still consuming RSS
- `Recovered: 962 MB` -- `malloc_trim(0)` got it back

## Build and run without Docker

Requires Linux with glibc and a C compiler.

```sh
git clone https://github.com/xroche/glibc-malloc-hoarding-reproducer.git
cd glibc-malloc-hoarding-reproducer

make
./reproducer --trim --verbose
```

## Options

```
--threads N       Worker threads (default: 16)
--index-mb N      MB of long-lived index per thread (default: 16)
--burst-mb N      MB of query data per burst per thread (default: 64)
--bursts N        Burst rounds per thread (default: 10)
--trim            Call malloc_trim(0) at end and measure recovery
--verbose         Print RSS during burst processing
```

Increase `--threads` and `--burst-mb` for a more dramatic effect. The hoarded
amount scales with both.

## How to read the results

| Metric | Meaning |
|--------|---------|
| `in-use (sbrk)` | Bytes actually used by the application |
| `free (sbrk)` | Bytes freed but held by glibc, still consuming RSS |
| `RSS after free` | Total physical memory used (should be close to in-use, but isn't) |
| `Recovered` | How much `malloc_trim(0)` got back (the gap that glibc should have released) |

If `free (sbrk)` is large and `Recovered` is close to it, the problem is
confirmed: glibc is holding freed memory that it could return.

## Why this happens

glibc's `free()` only trims memory from the top of each heap (via `systrim` or
`heap_trim`). Free chunks in the middle of a heap -- between live allocations --
keep their physical pages forever. The only function that releases interior
pages is `malloc_trim()`, which calls `madvise(MADV_DONTNEED)` on the
page-aligned interior of free chunks. But `malloc_trim` is never called
automatically.

## Tentative patch

A tentative glibc patch is in the [`patch/`](patch/) directory. It adds a
tunable `glibc.malloc.madvise_threshold` that makes `free()` call
`madvise(MADV_DONTNEED)` on interior free chunks above the threshold. With
threshold=64K, the reproducer RSS drops from 1247 MB to 296 MB.

## AI disclosure

This reproducer was written with AI assistance (Claude). The author reviewed
and tested all code.

## License

MIT. See [LICENSE](LICENSE).
