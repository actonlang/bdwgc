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
 * Test that a caller which holds the allocator lock until the marking of
 * the collection in progress completes runs the world-stopped steps of
 * that marking back to back, and completes it: the incremental mode with
 * the manual VDB and a time limit of 1 ms, a live tree of 32 MiB (on
 * 64-bit targets), and every collection full.  The test allocates until
 * a world stop leaves the marking of a collection incomplete, then calls
 * `GC_gcollect()`, which completes that marking before its own collection,
 * and then allocates again until the next collection marks and calls
 * `GC_finish_and_disable()`.  Both hold the allocator lock meanwhile, so
 * no client allocates from the heap, and spacing the steps would only keep
 * the lock held with the world running.  Where the collector spaces work
 * in time rather than by the bytes allocated, the spacing is a quarter of
 * the time limit at least (the final mark and the chunks of a set-up are
 * spaced by the time limit, the chunks of a finish by a quarter of it).
 * Thus, from the first stop of that marking to the one that completes it,
 * at least half of the times between two stops are under a quarter of the
 * time limit.  Not all of them: such a time also counts any wait of the
 * thread for a processor, or for a marker thread which waits for one,
 * which on a loaded host may last longer than a step.  Without the manual
 * VDB mode, the parallel marker (only with it the marking runs in steps)
 * or the clock, or if `NO_INCREMENTAL` is defined, the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"

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

/* The nodes of the live tree, of 4 words each (32 MiB on 64-bit targets). */
#define N_NODES (1 << 20)

/* The time limit, in milliseconds. */
#define TIME_LIMIT_MS 1

/* The world stops timed at most. */
#define MAX_STOPS 10000

/* The rounds of allocation at most before a collection marks. */
#define MAX_ROUNDS 100000000UL

#if defined(PARALLEL_MARK) && !defined(NO_CLOCK) && !defined(NO_INCREMENTAL) \
    && !defined(_WIN32)
/*
 * The world stops (their start and end in microseconds, and whether the
 * marking of a collection completed in them), and their count.
 */
static double stop_start[MAX_STOPS];
static double stop_end[MAX_STOPS];
static int completes[MAX_STOPS];
static volatile int n_stops;

struct node {
  struct node *left;
  struct node *right;
  GC_word value;
  GC_word pad;
};

static struct node *volatile root;
static void *volatile junk;

static double
now_us(void)
{
  struct timespec ts;

  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* Called with the allocator lock held. */
static void GC_CALLBACK
on_event(GC_EventType e)
{
  int i = n_stops;

  if (i >= MAX_STOPS)
    return;
  if (GC_EVENT_PRE_STOP_WORLD == e) {
    stop_start[i] = now_us();
    completes[i] = 0;
  } else if (GC_EVENT_MARK_END == e) {
    completes[i] = 1;
  } else if (GC_EVENT_POST_START_WORLD == e) {
    stop_end[i] = now_us();
    n_stops = i + 1;
  }
}

static struct node *
build(GC_word first, GC_word n)
{
  struct node *t;
  GC_word left_n;

  if (0 == n)
    return NULL;
  t = (struct node *)GC_MALLOC(sizeof(struct node));
  CHECK(t != NULL);
  t->value = first;
  left_n = (n - 1) / 2;
  GC_PTR_STORE_AND_DIRTY(&t->left, build(first + 1, left_n));
  GC_PTR_STORE_AND_DIRTY(&t->right, build(first + 1 + left_n, n - 1 - left_n));
  return t;
}

/*
 * Allocate short-lived objects until a world stop leaves the marking of
 * a collection incomplete.
 */
static void
allocate_until_marking(void)
{
  unsigned long i;

  for (i = 0; i < MAX_ROUNDS; i++) {
    int n = n_stops;

    if (n > 0 && !completes[n - 1])
      return;
    junk = GC_MALLOC(sizeof(struct node));
    CHECK(junk != NULL);
  }
  CHECK(0);
}

/*
 * Check the stops from `first` until the one that completed the marking:
 * at least half of the times between two of them are under a quarter of
 * the time limit.
 */
static void
check_back_to_back(const char *what, int first)
{
  double stopped = 0, running = 0;
  int i, n_gaps = 0, n_long = 0;

  for (i = first; i < n_stops; i++) {
    stopped += stop_end[i] - stop_start[i];
    if (completes[i])
      break;
    if (i + 1 < n_stops) {
      double gap = stop_start[i + 1] - stop_end[i];

      running += gap;
      n_gaps++;
      if (gap >= TIME_LIMIT_MS * 1000.0 / 4)
        n_long++;
    }
  }
  printf("%s: %d stops, %.0f us stopped, %.0f us running,"
         " %d of %d times between stops at least %d us\n",
         what, i - first + 1, stopped, running, n_long, n_gaps,
         TIME_LIMIT_MS * 1000 / 4);
  CHECK(i < n_stops);
  CHECK(2 * n_long <= n_gaps);
}

int
main(void)
{
  int first;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  GC_enable_incremental();
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The incremental mode with the manual VDB is unavailable,"
           " test skipped\n");
    return 0;
  }
  GC_set_time_limit(TIME_LIMIT_MS);
  GC_set_full_freq(0);
  root = build(0, N_NODES);
  GC_gcollect();
  GC_set_on_collection_event(on_event);

  allocate_until_marking();
  first = n_stops;
  GC_gcollect();
  check_back_to_back("GC_gcollect", first);

  allocate_until_marking();
  first = n_stops;
  GC_finish_and_disable();
  check_back_to_back("GC_finish_and_disable", first);
  GC_enable();
  GC_set_on_collection_event(0);
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The parallel marker, the clock or the incremental mode is"
         " unavailable, test skipped\n");
  return 0;
}
#endif
