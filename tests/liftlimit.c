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
 * Test that the work of a collection left to a later chunk (the set-up,
 * the finish, or the unmapping of the finish) completes at the next call
 * of `GC_collect_a_little()` once the time limit is lifted
 * (`GC_TIME_UNLIMITED`): the incremental mode with the manual VDB, every
 * collection full and a time limit of 10 microseconds.  In each of three
 * parts, the test makes a collection due, and the time limit becomes 1 ns
 * at a point of that collection: at its start (the test sets it before it
 * makes the collection due), at the end of its marking
 * (`GC_EVENT_MARK_END`) or at the end of the rest of its finish
 * (`GC_EVENT_RECLAIM_END`), where the collection event callback sets it,
 * holding the allocator lock.  Half of a time limit of 1 ns has passed at
 * any reading of the clock, so the work after that point does not start in
 * the same hold of the allocator lock: the set-up does not complete in its
 * first chunk or does not start the first world-stopped mark, the finish
 * does not start in the hold of the final mark, and the unmapping does not
 * start in the chunk that runs the rest of the finish.  The test calls
 * `GC_collect_a_little()` without allocating until the time limit is 1 ns
 * (the first call starts the collection), lifts the time limit and calls
 * `GC_collect_a_little()` once more: without a time limit the next chunk
 * is due at once and completes the collection, so that call must report no
 * work left.  The test keeps every other one of many small objects, so
 * that the set-up has blocks to sweep and the finish has objects to free.
 * The call that sets the time limit to 1 ns must leave work: the set-up in
 * the first part, the finish in the second, and the unmapping in the third
 * with memory unmapping (unless `GC_UNMAP_THRESHOLD` is 0, which turns it
 * off), the last two unless the write-barrier checker runs (the finish
 * then runs at once).  Without the manual VDB mode, the parallel marker
 * (only with it the set-up and the finish run in chunks) or the clock, or
 * if `NO_INCREMENTAL` is defined, the test is skipped.
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
 * and whether the time limit is 1 ns.  Updated by `on_event()`, which the
 * collector calls holding the allocator lock.
 */
static volatile int lower_at = -1;
static volatile int lowered;

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
  if ((int)e == lower_at) {
    set_time_limit_ns(1);
    lower_at = -1;
    lowered = 1;
  }
}

/*
 * Run the part `part`.  Store whether collection work was left after the
 * call of `GC_collect_a_little()` that set the time limit to 1 ns into
 * `*pending`, and return whether work was left after the call without
 * a time limit.
 */
static int
run_part(int part, int *pending)
{
  time_t start;
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
    *pending = GC_collect_a_little();
  } while (!lowered);
  GC_set_time_limit(GC_TIME_UNLIMITED);
  left = GC_collect_a_little();
  printf("%s: work %s at the time limit of 1 ns, %s after a call without"
         " a time limit\n",
         parts[part].work, *pending ? "left" : "complete",
         left ? "left" : "complete");
  return left;
}

int
main(void)
{
  int pending[N_PARTS];
  int i, part, n_left = 0;

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
  for (part = 0; part < N_PARTS; part++)
    n_left += run_part(part, &pending[part]);
  GC_set_on_collection_event(0);
  if (!pending[0]) {
    fprintf(stderr, "No set-up was left to a later chunk\n");
    exit(1);
  }
  if (NULL == getenv("GC_WBAR_VERIFY")) {
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
  }
  if (n_left > 0) {
    fprintf(stderr,
            "Collection work was left after a call without a time limit"
            " in %d of %d parts\n",
            n_left, N_PARTS);
    exit(1);
  }
  printf("SUCCEEDED\n");
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
