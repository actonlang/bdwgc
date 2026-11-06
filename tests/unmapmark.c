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
 * Test that no free block is unmapped while a collection is in progress,
 * also when an allocation merges free blocks: the incremental mode with
 * the manual VDB and a time limit of 10 microseconds, every collection
 * full.  In each cycle, the test allocates a large object with pointers,
 * kept by a root, right before another large object, which it frees
 * explicitly and unmaps.  It starts a collection and runs steps until the
 * kept object is marked, and a few more, so that the marker scans it in
 * pieces and the rest of it stays on the mark stack (the case requires
 * a new object that only the last word of the kept object references to
 * be unmarked still: a step that empties the mark stack leaves the final
 * mark of the collection due, with nothing left to read), then frees that
 * object explicitly as well: its block is free and mapped, next to
 * a larger unmapped one.  Then, with collections disabled, so that the
 * allocation does no marking work, it allocates an object larger than
 * every free block, which merges the free blocks (and grows the heap),
 * and runs the collection to its end.  The unmapped bytes must not grow
 * while the collection is in progress.  `GC_free` drops the rest of the
 * freed object from the mark stack, but an unmapping then would rely on
 * no range on it lying in any free block: the next steps read each range,
 * and where unmapped memory is inaccessible (e.g. on macOS), that faults.
 * The case must be reached in at least one cycle (more cycles run while
 * it is not), unless the marking cannot stop at the time limit.  Without
 * the manual VDB mode or memory unmapping the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc/gc_mark.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define MiB ((size_t)1 << 20)

/*
 * Whether the marking can stop at the time limit and span steps.  On
 * Windows the clock of the collector counts milliseconds: a step runs
 * until the clock advances, not for 10 microseconds, and often marks to
 * the end.
 */
#if defined(NO_CLOCK) || defined(_WIN32)
#  define MARK_STEPS 0
#else
#  define MARK_STEPS 1
#endif

/* The object scanned in pieces, and the object after it that is unmapped. */
#define SCANNED_BYTES (8 * MiB)
#define UNMAPPED_BYTES (24 * MiB)

/* The last word of the scanned object, which references the tail object. */
#define TAIL_INDEX (SCANNED_BYTES / sizeof(GC_word) - 1)

/*
 * The cycles to run, and the cycles at most while the case is not reached
 * (e.g. if the large objects are not adjacent).
 */
#define N_CYCLES 3
#define MAX_CYCLES 8

/* The largest block size: a smaller gap between objects holds no block. */
#define MAX_BLOCK (64 * 1024)

/*
 * The time `run_cycle()` waits at most, in seconds, in each of its loops:
 * for the marking to start, for the scanned object to be marked and for
 * the steps after it.  The loops allocate nothing: once a chunk of the
 * set-up of the collection does not complete the set-up, or ends after
 * half of the time limit, the next chunk or the first world-stopped mark
 * is due only when the time limit has passed since the end of that chunk,
 * and a step is due only once the world has run for as long as the latest
 * slice stopped it (for less above a share of half).  The calls of
 * `GC_collect_a_little()` in between do nothing, so a bound on their
 * number would not bound the time, and a slice that stops the world for
 * long (e.g. on a loaded machine) would exhaust it.  The clock of the
 * collector counts milliseconds on Windows (`GetTickCount64()` or
 * `GetTickCount()`), so there a wait lasts until a later tick of it.
 */
#define MAX_WAIT_S 60

static GC_word *volatile scanned;

/*
 * The marks started and ended, and the world stops of the world-stopped
 * marks and steps.  Updated holding the allocator lock.
 */
static volatile unsigned long n_mark_starts, n_mark_ends, n_stops;

static void *GC_CALLBACK
is_marked(void *p)
{
  return GC_is_marked(p) ? p : NULL;
}

/*
 * Store a new object, the tail object, into the last word of the scanned
 * object, its only reference: while it is unmarked, the marker has not
 * scanned the end of the scanned object.
 */
static void
store_tail(void)
{
  const void *tail = GC_MALLOC(sizeof(GC_word));

  CHECK(tail != NULL);
  scanned[TAIL_INDEX] = (GC_word)tail;
  GC_END_STUBBORN_CHANGE(scanned + TAIL_INDEX);
}

/* Called indirectly, so that no frame of the caller holds the address. */
static void (*volatile store_tail_fn)(void) = store_tail;

static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_MARK_START == e)
    n_mark_starts++;
  else if (GC_EVENT_MARK_END == e)
    n_mark_ends++;
  else if (GC_EVENT_POST_STOP_WORLD == e)
    n_stops++;
}

/*
 * The cycles that found the scanned object next to an unmapped block, and
 * the cycles whose marking was still in progress after the step that
 * started it.
 */
static int n_set_up, n_spanned;

/*
 * Run a cycle with `steps` steps before the scanned object is freed.
 * Return whether the object was freed, and the blocks merged, while the
 * collection marked.
 */
static int
run_cycle(int steps)
{
  GC_word *after;
  unsigned long starts, ends, stops;
  time_t start;
  int reached;

  scanned = (GC_word *)GC_MALLOC(SCANNED_BYTES);
  after = (GC_word *)GC_MALLOC_ATOMIC(UNMAPPED_BYTES);
  CHECK(scanned != NULL && after != NULL);
  if ((GC_word)after < (GC_word)scanned + GC_size(scanned)
      || (GC_word)after - (GC_word)scanned - GC_size(scanned) >= MAX_BLOCK) {
    /* The blocks are not adjacent; nothing to test in this cycle. */
    GC_FREE(after);
    return 0;
  }
  GC_FREE(after);
  after = NULL;
  store_tail_fn();
  GC_gcollect_and_unmap();
  if (GC_get_unmapped_bytes() < UNMAPPED_BYTES)
    return 0;
  n_set_up++;

  starts = n_mark_starts;
  ends = n_mark_ends;
  start = time(NULL);
  GC_start_incremental_collection();
  while (n_mark_starts == starts) {
    CHECK(difftime(time(NULL), start) < MAX_WAIT_S);
    (void)GC_collect_a_little();
  }
  if (n_mark_ends == ends)
    n_spanned++;
  start = time(NULL);
  while (n_mark_starts == starts + 1 && n_mark_ends == ends
         && NULL == GC_call_with_alloc_lock(is_marked, scanned)) {
    CHECK(difftime(time(NULL), start) < MAX_WAIT_S);
    (void)GC_collect_a_little();
  }
  stops = n_stops;
  start = time(NULL);
  while (n_stops - stops < (unsigned long)steps && n_mark_starts == starts + 1
         && n_mark_ends == ends) {
    CHECK(difftime(time(NULL), start) < MAX_WAIT_S);
    (void)GC_collect_a_little();
  }
  reached = n_mark_starts == starts + 1 && n_mark_ends == ends
            && NULL
                   == GC_call_with_alloc_lock(is_marked,
                                              (void *)scanned[TAIL_INDEX]);
  if (reached) {
    size_t unmapped, total;
    void *big;
    GC_warn_proc warn_proc;

    GC_FREE(scanned);
    scanned = NULL;
    unmapped = GC_get_unmapped_bytes();
    total = GC_get_heap_size() + unmapped;
    GC_disable();
    big = GC_MALLOC_ATOMIC(total + 16 * MiB);
    /* Silence the warning that the heap grew while collections were off. */
    warn_proc = GC_get_warn_proc();
    GC_set_warn_proc(GC_ignore_warn_proc);
    GC_enable();
    GC_set_warn_proc(warn_proc);
    CHECK(big != NULL);
    reached = n_mark_ends == ends;
    if (reached && GC_get_unmapped_bytes() > unmapped) {
      fprintf(stderr, "Unmapped %lu KiB while marking\n",
              (unsigned long)((GC_get_unmapped_bytes() - unmapped) >> 10));
      exit(1);
    }
    GC_FREE(big);
  }
  {
    struct GC_timeval_s tv = GC_get_time_limit_tv();

    /*
     * Complete the collection without the time limit, which its steps and
     * its final marks keep otherwise.
     */
    GC_set_time_limit(GC_TIME_UNLIMITED);
    while (GC_collect_a_little()) {
      /* Empty. */
    }
    GC_set_time_limit_tv(tv);
  }
  return reached;
}

int
main(void)
{
  struct GC_timeval_s tv;
  int cycle, n_reached = 0;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  GC_enable_incremental();
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  CHECK(GC_MALLOC_ATOMIC(MiB) != NULL);
  GC_gcollect_and_unmap();
  if (0 == GC_get_unmapped_bytes()) {
    printf("Memory unmapping is unavailable, test skipped\n");
    return 0;
  }
  tv.tv_ms = 0;
  tv.tv_nsec = 10 * 1000;
  GC_set_time_limit_tv(tv);
  GC_set_full_freq(0);
  CHECK(GC_expand_hp(2 * (SCANNED_BYTES + UNMAPPED_BYTES)));
  GC_set_on_collection_event(on_event);
  for (cycle = 0; cycle < N_CYCLES || (0 == n_reached && cycle < MAX_CYCLES);
       cycle++)
    n_reached += run_cycle(cycle % N_CYCLES);
  if (0 == n_set_up) {
    printf("The scanned object was never next to an unmapped block,"
           " test skipped\n");
    return 0;
  }
  if (0 == n_spanned) {
    printf("The marking never stopped at the time limit, test skipped\n");
    return 0;
  }
  if (0 == n_reached && MARK_STEPS) {
    fprintf(stderr, "No object was freed while marking\n");
    return 1;
  }
  printf("SUCCEEDED (freed while marking in %d of %d cycles)\n", n_reached,
         cycle);
  return 0;
}
