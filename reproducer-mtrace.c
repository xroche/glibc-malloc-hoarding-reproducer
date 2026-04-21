/* glibc malloc hoarding — mtrace-instrumented reproducer.
 *
 * Mirrors tst-madvise-threshold.c but standalone (no glibc test harness
 * dependency) and with mtrace() hooked in so every malloc/realloc/free
 * is logged via the MALLOC_TRACE mechanism.  Written in response to
 * Rocket Ma's request on BZ #33886 to help diagnose the allocation
 * pattern.
 *
 * Build:
 *   gcc -O2 -g -o reproducer-mtrace reproducer-mtrace.c
 *
 * Run (Ubuntu 22.04+, mtrace entry points live in libc_malloc_debug.so):
 *   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libc_malloc_debug.so.0 \
 *     MALLOC_TRACE=/tmp/reproducer.mtrace \
 *     ./reproducer-mtrace
 *
 * Analyze:
 *   mtrace ./reproducer-mtrace /tmp/reproducer.mtrace
 *
 * The allocation pattern is deliberately simple and small so that the
 * resulting trace is ~5000 lines rather than the ~millions a full
 * 10 GB churn workload would produce.
 */

#define _GNU_SOURCE
#include <malloc.h>
#include <mcheck.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N_INDEX    200       /* long-lived allocations, kept alive */
#define N_FILLER   2000      /* short-lived allocations, freed     */
#define INDEX_SIZE 1024      /* 1 KB per index entry               */
#define FILLER_SIZE (64 * 1024)  /* 64 KB per filler entry         */

static long
get_rss_mb (void)
{
  FILE *f = fopen ("/proc/self/statm", "r");
  if (!f)
    return -1;
  long pages;
  if (fscanf (f, "%*d %ld", &pages) != 1)
    {
      fclose (f);
      return -1;
    }
  fclose (f);
  return pages * (sysconf (_SC_PAGESIZE) / 1024) / 1024;
}

/* If MALLOC_INFO_PREFIX is set in the environment, write the
 * malloc_info(0, fp) XML dump to ${MALLOC_INFO_PREFIX}${phase}.xml
 * so reviewers can compare allocator state at each phase between
 * system and patched glibc.  No-op if the env var is unset. */
static void
dump_malloc_info (const char *phase)
{
  const char *prefix = getenv ("MALLOC_INFO_PREFIX");
  if (!prefix || !*prefix)
    return;
  char path[1024];
  snprintf (path, sizeof path, "%s%s.xml", prefix, phase);
  FILE *f = fopen (path, "w");
  if (!f)
    {
      fprintf (stderr, "  malloc_info dump: cannot open %s\n", path);
      return;
    }
  malloc_info (0, f);
  fclose (f);
  printf ("  malloc_info written to %s\n", path);
}

/* Rocket Ma's note: install a SIGINT handler so that mtrace's
 * deferred flush still gets to write out the trace file.  We don't
 * ever raise SIGINT here, but the reproducer is short enough that
 * it doesn't matter.  Included for completeness.  */
static void
handle_sigint (int sig)
{
  (void) sig;
  muntrace ();
  _exit (130);
}

int
main (void)
{
  mtrace ();

  struct sigaction sa = { 0 };
  sa.sa_handler = handle_sigint;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGINT, &sa, NULL);

  void *index_ptrs[N_INDEX];
  void *filler_ptrs[N_FILLER];

  printf ("Phase 1: allocate %d index (1 KB each) + %d filler (64 KB each),\n",
          N_INDEX, N_FILLER);
  printf ("         interleaved to create fragmentation.\n");

  int idx = 0;
  for (int i = 0; i < N_FILLER; i++)
    {
      if (idx < N_INDEX && i % (N_FILLER / N_INDEX) == 0)
        {
          index_ptrs[idx] = malloc (INDEX_SIZE);
          if (!index_ptrs[idx])
            {
              fprintf (stderr, "index malloc #%d failed\n", idx);
              return 1;
            }
          memset (index_ptrs[idx], 0xAA, INDEX_SIZE);
          idx++;
        }
      filler_ptrs[i] = malloc (FILLER_SIZE);
      if (!filler_ptrs[i])
        {
          fprintf (stderr, "filler malloc #%d failed\n", i);
          return 1;
        }
      memset (filler_ptrs[i], 0xBB, FILLER_SIZE);
    }

  printf ("  RSS after allocation: %ld MB\n", get_rss_mb ());
  dump_malloc_info ("phase1");

  printf ("Phase 2: free all filler (index stays alive, pinning the heap).\n");
  for (int i = 0; i < N_FILLER; i++)
    free (filler_ptrs[i]);

  printf ("  RSS after free:       %ld MB\n", get_rss_mb ());
  dump_malloc_info ("phase2");

  printf ("Phase 3: cleanup (free index so mtrace sees balanced pairs).\n");
  for (int i = 0; i < N_INDEX; i++)
    free (index_ptrs[i]);
  dump_malloc_info ("phase3");

  muntrace ();
  printf ("Done.\n");
  return 0;
}
