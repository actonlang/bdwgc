/*
 * Copyright (c) 2026 Acton contributors
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

/*
 * Test that the work of a collection left to later chunks (the set-up,
 * the finish, or the unmapping of the finish) completes while the time
 * limit stays at 1 ns: the incremental mode with the manual VDB, every
 * collection full and a time limit of 10 microseconds.  In each of three
 * parts, the test makes a collection due, and the time limit becomes 1 ns
 * at a point of that collection: at its start (the test sets it before it
 * makes the collection due), at the end of its marking
 * (`GC_EVENT_MARK_END`) or at the end of the rest of its finish
 * (`GC_EVENT_RECLAIM_END`), where the collection event callback sets it,
 * holding the allocator lock.  Half of a time limit of 1 ns has passed at
 * any reading of the clock, so the first world-stopped mark does not start
 * in the chunk that completes the set-up, the finish does not start in the
 * hold of the final mark, and the walk of the blocks, the rest of the
 * finish and the unmapping do not start in the chunk that completes the
 * phase before them: each starts a chunk of its own, which must make
 * progress in it.  The test calls `GC_collect_a_little()` without
 * allocating until the time limit is 1 ns (the first call starts the
 * collection), then until it reports no work left, at most `MAX_CALLS`
 * times: a chunk of the set-up or of the finish is then due only once the
 * time limit (a quarter of it before the unmapping) has passed since the
 * end of the previous chunk, which holds at any reading of the clock.  The
 * work must complete in every part.  The call that sets the time limit to
 * 1 ns must leave work: the set-up in the first part, the finish in the
 * second, and the unmapping in the third with memory unmapping (unless
 * `GC_UNMAP_THRESHOLD` is 0, which turns it off), the last two unless the
 * write-barrier checker runs (the finish then runs at once).  In the first
 * two parts, unless the write-barrier checker runs, the rest of the finish
 * must end at least 3 calls after the call that starts the finish (the
 * finish, the walk of the blocks and the rest of the finish each start
 * a chunk of their own).  The test keeps every other one of many small
 * objects, so that the set-up has blocks to sweep and the finish has
 * objects to free.  Without the manual VDB mode, the parallel marker (only
 * with it the set-up and the finish run in chunks) or the clock, or if
 * `NO_INCREMENTAL` is defined, the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#if defined(PARALLEL_MARK) && !defined(NO_CLOCK)

/* The objects kept, each allocated before a garbage one, and their size. */
#  define N_KEPT 4096
#  define OBJ_BYTES 64

/*
 * The time that the test waits at most for a collection to reach the point
 * of a part, in seconds.
 */
#  define MAX_WAIT_S 60

/*
 * The calls of `GC_collect_a_little()` after the one that sets the time
 * limit to 1 ns, at most, until the work of the part is complete (fewer
 * than 2000 in each part on macOS and Linux arm64).
 */
#  define MAX_CALLS 1000000UL

#  define N_PARTS 3

/*
 * The parts: the work left to a later chunk, and the event at which the
 * time limit becomes 1 ns (-1: before the collection starts).
 */
static const struct {
  const char *work;
  int event;
} parts[N_PARTS] = { { "set-up", -1 },
                     { "finish", GC_EVENT_MARK_END },
                     { "unmapping", GC_EVENT_RECLAIM_END } };

static void *volatile kept[N_KEPT];

/*
 * The event at which `on_event()` sets the time limit to 1 ns (-1: none),
 * whether the time limit is 1 ns, the calls of `GC_collect_a_little()` by
 * the test, and that count at the start of the latest finish and at the
 * end of its rest.  Updated by `on_event()`, which the collector calls
 * holding the allocator lock, except for the calls, which the test counts.
 */
static volatile int lower_at = -1;
static volatile int lowered;
static volatile unsigned long n_calls;
static volatile unsigned long finish_start_call;
static volatile unsigned long finish_end_call;

static void
set_time_limit_ns(unsigned long ns)
{
  struct GC_timeval_s tv;

  tv.tv_ms = ns / 1000000;
  tv.tv_nsec = ns % 1000000;
  GC_set_time_limit_tv(tv);
}

static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_RECLAIM_START == e)
    finish_start_call = n_calls;
  if (GC_EVENT_RECLAIM_END == e)
    finish_end_call = n_calls;
  if ((int)e == lower_at) {
    set_time_limit_ns(1);
    lower_at = -1;
    lowered = 1;
  }
}

/* Call `GC_collect_a_little()`, counting the call. */
static int
collect_a_little(void)
{
  n_calls++;
  return GC_collect_a_little();
}

/*
 * Run the part `part`.  Store whether collection work was left after the
 * call of `GC_collect_a_little()` that set the time limit to 1 ns into
 * `*pending`, and the calls after it until no work was left (or
 * `MAX_CALLS`) into `*pcalls`.  Return whether work was left after them.
 */
static int
run_part(int part, int *pending, unsigned long *pcalls)
{
  time_t start;
  unsigned long n;
  int left;

  /* Complete the collection of the previous part. */
  GC_gcollect();
  set_time_limit_ns(10 * 1000);
  lowered = 0;
  if (parts[part].event < 0) {
    set_time_limit_ns(1);
    lowered = 1;
  }
  lower_at = parts[part].event;
  /*
   * With the collections disabled, `GC_start_incremental_collection()`
   * makes a collection due without starting it, so that the first call of
   * `GC_collect_a_little()` below starts it.
   */
  GC_disable();
  GC_start_incremental_collection();
  GC_enable();
  start = time(NULL);
  do {
    CHECK(difftime(time(NULL), start) < MAX_WAIT_S);
    *pending = collect_a_little();
  } while (!lowered);
  left = *pending;
  for (n = 0; left && n < MAX_CALLS; n++)
    left = collect_a_little();
  *pcalls = n;
  printf("%s: work %s at the time limit of 1 ns, %s after %lu more calls\n",
         parts[part].work, *pending ? "left" : "complete",
         left ? "still left" : "complete", n);
  return left;
}

int
main(void)
{
  int pending[N_PARTS];
  unsigned long calls[N_PARTS];
  unsigned long finish_calls[N_PARTS];
  int i, part, n_left = 0;
  int checker = getenv("GC_WBAR_VERIFY") != NULL;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
#  ifndef NO_INCREMENTAL
  GC_enable_incremental();
#  endif
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The incremental mode with the manual VDB is unavailable,"
           " test skipped\n");
    return 0;
  }
  GC_set_full_freq(0);
  for (i = 0; i < 2 * N_KEPT; i++) {
    void *p = GC_MALLOC(OBJ_BYTES);

    CHECK(p != NULL);
    if (i % 2 == 0)
      kept[i / 2] = p;
  }
  GC_set_on_collection_event(on_event);
  for (part = 0; part < N_PARTS; part++) {
    n_left += run_part(part, &pending[part], &calls[part]);
    finish_calls[part] = finish_end_call - finish_start_call;
  }
  GC_set_on_collection_event(0);
  if (n_left > 0) {
    fprintf(stderr,
            "Collection work was left after %lu calls at the time limit"
            " of 1 ns in %d of %d parts\n",
            MAX_CALLS, n_left, N_PARTS);
    exit(1);
  }
  if (!pending[0]) {
    fprintf(stderr, "No set-up was left to a later chunk\n");
    exit(1);
  }
  if (!checker) {
    if (!pending[1]) {
      fprintf(stderr, "No finish was left to a later chunk\n");
      exit(1);
    }
#  ifdef USE_MUNMAP
    /* The collector reads "0" as no unmapping. */
    if (!pending[2]
        && (NULL == getenv("GC_UNMAP_THRESHOLD")
            || strcmp(getenv("GC_UNMAP_THRESHOLD"), "0") != 0)) {
      fprintf(stderr, "No unmapping was left to a later chunk\n");
      exit(1);
    }
#  endif
    for (part = 0; part < 2; part++) {
      if (finish_calls[part] < 3) {
        fprintf(stderr,
                "The rest of the finish ended %lu calls after the start of"
                " the finish in the %s part\n",
                finish_calls[part], parts[part].work);
        exit(1);
      }
    }
  }
  printf("SUCCEEDED (%lu, %lu and %lu calls after the time limit of 1 ns)\n",
         calls[0], calls[1], calls[2]);
  return 0;
}

#else
int
main(void)
{
  printf("The parallel marker or the clock is unavailable, test skipped\n");
  return 0;
}
#endif
