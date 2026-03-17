/*
 * glibc malloc memory hoarding reproducer
 *
 * Simulates the Algolia search engine allocation pattern:
 * - Worker threads allocate a mix of long-lived "index" data and
 *   short-lived "query" data, interleaved within the same arena heaps
 * - After all query data is freed, the arena heaps retain physical memory
 *   because index chunks pin heap segments
 * - Calling malloc_trim(0) recovers the hoarded memory via MADV_DONTNEED
 *
 * The key pattern: each thread does a "burst" phase where it allocates
 * a large amount of both index and query data.  The index data stays alive
 * while the query data is freed.  This leaves large free holes between
 * live index chunks in the arena heaps.
 *
 * Usage: ./reproducer [options]
 *   --threads N       Number of worker threads (default: 16)
 *   --index-mb N      MB of long-lived index data per thread (default: 16)
 *   --burst-mb N      MB of query data per burst per thread (default: 64)
 *   --bursts N        Number of burst rounds per thread (default: 10)
 *   --trim            Call malloc_trim(0) at the end and measure recovery
 *   --verbose         Print per-phase RSS
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include <getopt.h>
#include <stdatomic.h>
#include <time.h>
#include <gnu/libc-version.h>

/* ---------- configuration ---------- */

static int    cfg_threads       = 16;
static int    cfg_index_mb      = 16;     /* per thread */
static int    cfg_burst_mb      = 64;     /* per thread per burst */
static int    cfg_bursts        = 10;
static int    cfg_do_trim       = 0;
static int    cfg_verbose       = 0;

/* ---------- global index tracking ---------- */

#define MAX_INDEX_CHUNKS (4 * 1024 * 1024)
static void          *index_chunks[MAX_INDEX_CHUNKS];
static _Atomic int    index_count = 0;

static pthread_barrier_t start_barrier;

/* ---------- helpers ---------- */

static long
get_rss_kb (void)
{
  FILE *f = fopen ("/proc/self/statm", "r");
  if (!f)
    return -1;
  long pages;
  if (fscanf (f, "%*d %ld", &pages) != 1)
    pages = -1;
  fclose (f);
  return pages * (sysconf (_SC_PAGESIZE) / 1024);
}

static void
report_rss (const char *phase)
{
  long rss = get_rss_kb ();
  if (rss >= 0)
    printf ("  %-36s RSS: %7ld KB  (%ld MB)\n",
            phase, rss, rss / 1024);
}

static inline uint64_t
xrand (uint64_t *state)
{
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/*
 * Allocation sizes stay below the default mmap threshold (128 KB) so
 * everything goes through arena heaps, where the hoarding occurs.
 */

/* Query: varied sizes, all under 120 KB */
static size_t
random_query_size (uint64_t *rng)
{
  uint64_t r = xrand (rng);
  unsigned bucket = r % 100;
  uint64_t r2 = xrand (rng);

  if (bucket < 40)
    return 64 + (r2 % 448);                  /* 64-512 */
  else if (bucket < 70)
    return 512 + (r2 % (4 * 1024 - 512));    /* 512-4K */
  else if (bucket < 90)
    return 4 * 1024 + (r2 % (32 * 1024 - 4 * 1024));  /* 4K-32K */
  else
    return 32 * 1024 + (r2 % (120 * 1024 - 32 * 1024)); /* 32K-120K */
}

/* Index: medium-sized, under 120 KB */
static size_t
random_index_size (uint64_t *rng)
{
  uint64_t r = xrand (rng);
  unsigned bucket = r % 100;
  uint64_t r2 = xrand (rng);

  if (bucket < 30)
    return 512 + (r2 % (2 * 1024 - 512));
  else if (bucket < 60)
    return 2 * 1024 + (r2 % (16 * 1024 - 2 * 1024));
  else if (bucket < 85)
    return 16 * 1024 + (r2 % (64 * 1024 - 16 * 1024));
  else
    return 64 * 1024 + (r2 % (120 * 1024 - 64 * 1024));
}

/* ---------- worker thread ---------- */

struct worker_arg
{
  int id;
};

/*
 * Each worker does cfg_bursts rounds.  In each round:
 *
 * 1. Allocate a batch of index chunks (long-lived) interleaved with
 *    query chunks (short-lived).  The interleaving is critical: it
 *    means index and query chunks are adjacent in the heap.
 *
 * 2. Free all query chunks from this round.  This creates free holes
 *    between live index chunks in the arena heap.
 *
 * After all rounds, the arena contains index chunks scattered among
 * large free regions.  The free regions consume RSS because glibc
 * doesn't call madvise(MADV_DONTNEED) on them.
 */
static void *
query_worker (void *arg)
{
  struct worker_arg *wa = arg;
  uint64_t rng = 0xabcdef0123456789ULL ^ ((uint64_t) wa->id * 0x9e3779b97f4a7c15ULL);

  pthread_barrier_wait (&start_barrier);

  size_t index_target = (size_t) cfg_index_mb * 1024 * 1024;
  size_t burst_target = (size_t) cfg_burst_mb * 1024 * 1024;
  size_t index_per_burst = index_target / cfg_bursts;

  /* Temporary storage for query pointers within a burst. */
  int max_ptrs = 100000;
  void **query_ptrs = malloc (max_ptrs * sizeof (void *));
  if (!query_ptrs)
    return NULL;

  for (int b = 0; b < cfg_bursts; b++)
    {
      size_t idx_done = 0;
      size_t qry_done = 0;
      int qry_count = 0;

      /* Interleaved allocation: alternate between index and query chunks.
         This scatters index data among query data in the heap. */
      while (qry_done < burst_target)
        {
          /* Allocate 1 index chunk for every ~4 query chunks. */
          if (idx_done < index_per_burst)
            {
              size_t sz = random_index_size (&rng);
              void *p = malloc (sz);
              if (p)
                {
                  memset (p, 0xAA, sz);
                  int idx = atomic_fetch_add (&index_count, 1);
                  if (idx < MAX_INDEX_CHUNKS)
                    index_chunks[idx] = p;
                  idx_done += sz;
                }
            }

          /* Allocate a few query chunks. */
          for (int i = 0; i < 4 && qry_done < burst_target; i++)
            {
              size_t sz = random_query_size (&rng);
              void *p = malloc (sz);
              if (p)
                {
                  memset (p, 0xCC, sz);
                  if (qry_count < max_ptrs)
                    query_ptrs[qry_count++] = p;
                  else
                    free (p);  /* overflow safety */
                  qry_done += sz;
                }
            }
        }

      /* Free all query chunks from this burst (scrambled order). */
      for (int i = qry_count - 1; i > 0; i--)
        {
          int j = xrand (&rng) % (i + 1);
          void *tmp = query_ptrs[i];
          query_ptrs[i] = query_ptrs[j];
          query_ptrs[j] = tmp;
        }
      for (int i = 0; i < qry_count; i++)
        free (query_ptrs[i]);

      if (cfg_verbose && wa->id == 0 && (b == 0 || b == cfg_bursts - 1))
        {
          char buf[64];
          snprintf (buf, sizeof buf, "Thread 0, burst %d/%d", b + 1, cfg_bursts);
          report_rss (buf);
        }
    }

  free (query_ptrs);
  return NULL;
}

/* ---------- main ---------- */

static void
usage (const char *prog)
{
  fprintf (stderr,
    "Usage: %s [options]\n"
    "  --threads N       Worker threads (default: %d)\n"
    "  --index-mb N      MB of long-lived index per thread (default: %d)\n"
    "  --burst-mb N      MB of query data per burst per thread (default: %d)\n"
    "  --bursts N        Burst rounds per thread (default: %d)\n"
    "  --trim            Call malloc_trim(0) at end\n"
    "  --verbose         Print per-phase RSS\n",
    prog, cfg_threads, cfg_index_mb, cfg_burst_mb, cfg_bursts);
}

int
main (int argc, char **argv)
{
  static struct option long_opts[] = {
    { "threads",      required_argument, NULL, 't' },
    { "index-mb",     required_argument, NULL, 'i' },
    { "burst-mb",     required_argument, NULL, 'b' },
    { "bursts",       required_argument, NULL, 'n' },
    { "trim",         no_argument,       NULL, 'T' },
    { "verbose",      no_argument,       NULL, 'v' },
    { "help",         no_argument,       NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };

  int opt;
  while ((opt = getopt_long (argc, argv, "t:i:b:n:Tvh", long_opts, NULL)) != -1)
    {
      switch (opt)
        {
        case 't': cfg_threads = atoi (optarg); break;
        case 'i': cfg_index_mb = atoi (optarg); break;
        case 'b': cfg_burst_mb = atoi (optarg); break;
        case 'n': cfg_bursts = atoi (optarg); break;
        case 'T': cfg_do_trim = 1; break;
        case 'v': cfg_verbose = 1; break;
        case 'h': usage (argv[0]); return 0;
        default:  usage (argv[0]); return 1;
        }
    }

  int total_index = cfg_index_mb * cfg_threads;
  int total_burst = cfg_burst_mb * cfg_threads * cfg_bursts;

  printf ("=== glibc malloc hoarding reproducer ===\n");
  printf ("Threads: %d, Index: %d MB/thread (%d MB total)\n",
          cfg_threads, cfg_index_mb, total_index);
  printf ("Bursts: %d x %d MB/thread (%d MB total query data)\n",
          cfg_bursts, cfg_burst_mb, total_burst);
  printf ("glibc version: %s\n", gnu_get_libc_version ());
  printf ("\n");

  report_rss ("Initial");

  /* Run workers. */
  pthread_barrier_init (&start_barrier, NULL, cfg_threads);

  struct worker_arg *args = calloc (cfg_threads, sizeof *args);
  pthread_t *workers = calloc (cfg_threads, sizeof *workers);

  struct timespec t0, t1;
  clock_gettime (CLOCK_MONOTONIC, &t0);

  for (int i = 0; i < cfg_threads; i++)
    {
      args[i].id = i;
      pthread_create (&workers[i], NULL, query_worker, &args[i]);
    }
  for (int i = 0; i < cfg_threads; i++)
    pthread_join (workers[i], NULL);

  clock_gettime (CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec)
                   + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  printf ("\nDone in %.2f seconds.  Index: %d chunks\n", elapsed, index_count);
  report_rss ("After bursts (queries freed)");

  /* mallinfo2 summary */
  struct mallinfo2 mi = mallinfo2 ();
  printf ("\nmalloc stats:\n");
  printf ("  arena (sbrk):   %zu MB\n", mi.arena / (1024 * 1024));
  printf ("  in-use (sbrk):  %zu MB\n", mi.uordblks / (1024 * 1024));
  printf ("  free (sbrk):    %zu MB  <-- hoarded, not returned to OS\n",
          mi.fordblks / (1024 * 1024));
  printf ("  mmap'd:         %zu MB\n", mi.hblkhd / (1024 * 1024));

  if (cfg_do_trim)
    {
      printf ("\nCalling malloc_trim(0)...\n");
      long rss_before = get_rss_kb ();

      struct timespec tt0, tt1;
      clock_gettime (CLOCK_MONOTONIC, &tt0);
      malloc_trim (0);
      clock_gettime (CLOCK_MONOTONIC, &tt1);

      double trim_ms = (tt1.tv_sec - tt0.tv_sec) * 1000.0
                       + (tt1.tv_nsec - tt0.tv_nsec) / 1e6;
      long rss_after = get_rss_kb ();

      printf ("  malloc_trim took: %.1f ms\n", trim_ms);
      report_rss ("After malloc_trim");
      if (rss_before > 0 && rss_after > 0)
        printf ("  Recovered: %ld MB\n",
                (rss_before - rss_after) / 1024);
    }

  /* Free index data. */
  int ic = index_count;
  for (int i = 0; i < ic; i++)
    free (index_chunks[i]);

  report_rss ("After index freed");

  if (cfg_do_trim)
    {
      malloc_trim (0);
      report_rss ("After final trim");
    }

  free (args);
  free (workers);
  return 0;
}
