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
 * Test the pacing of the world-stopped steps of a marking: the incremental
 * mode with the manual VDB, a live tree of 2^20 nodes of 4 words each
 * (32 MiB on 64-bit targets), a client that allocates without pause, and
 * every collection full but in the last part.  The test times each world
 * stop by the collection events, by the clock of the collector: the elapsed
 * time, or the processor time where the collector counts its time limit in
 * it (see `PROCESSOR_TIME_LIMIT`), which does not advance while the threads
 * of the process wait for a processor.
 *
 * First, with a time limit of 1 ms, the test allocates until a world stop
 * leaves the marking of a collection incomplete, then calls `GC_gcollect()`,
 * which completes that marking before its own collection, and then
 * allocates again until the next collection marks and calls
 * `GC_finish_and_disable()`.  Both hold the allocator lock meanwhile, so no
 * client allocates from the heap, and spacing the steps would only keep the
 * lock held with the world running.  Where the collector spaces work in
 * time rather than by the bytes allocated, the spacing is a quarter of the
 * time limit at least (the final mark and the chunks of a set-up are spaced
 * by the time limit, the chunks of a finish by a quarter of it, and, while
 * the share of the time for which the world must be stopped is below half,
 * the steps by the length of the latest slice, the time limit at least after
 * a step that does not empty the mark stack).  Thus, from the first stop of
 * that marking to the one that completes it, at least half of the times
 * between two stops are under a quarter of the time limit.  Not all of them:
 * such a time also counts any wait of the thread for a processor, or for
 * a marker thread which waits for one, which on a loaded host may last longer
 * than a step.
 *
 * Then, at low pressure: there is no allocation budget, so the heap cap is
 * the live data plus half of the distance to the heap ceiling, far above
 * the heap of the test, thus the client must keep at least half of the
 * time.  It allocates until 12 more collections completed, at least 12 of
 * the world stops must leave the marking of a collection incomplete, and
 * after each of them the world must run about as long as it was stopped
 * before the next stop.
 *
 * Last, at high pressure: the memory limit is set to 2.5 times the bytes
 * in use, which puts the heap cap near the live data, and the time limit to
 * 10 microseconds; the heap in use (the mapped heap minus its free blocks)
 * must stay under the heap ceiling while the client allocates as fast.
 * These two parts are skipped if the memory limit is unknown.
 *
 * Without the manual VDB mode, the parallel marker (only with it the
 * marking runs in steps) or the clock, if `NO_INCREMENTAL` is defined, or
 * on Windows, where the clock of the collector counts milliseconds, the
 * test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef BSD_TIME
#  include <sys/resource.h>
#endif
#ifndef _WIN32
#  include <unistd.h> /*< for `_POSIX_TIMERS` */
#endif

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define MiB ((size_t)1 << 20)

/* The nodes of the live tree. */
#define N_NODES (1 << 20)

/* The time limit of the first two parts, in milliseconds. */
#define TIME_LIMIT_MS 1

/* The world stops timed at most, and the collections of a paced part. */
#define MAX_STOPS 100000
#define N_COLLECTIONS 12

/* The rounds of allocation at most before a collection marks. */
#define MAX_ROUNDS 100000000UL

/*
 * Defined where the collector counts its time limit in processor time (see
 * `GET_TIME()` in `include/private/gc_priv.h`): in the user time of the
 * process with `BSD_TIME`, and in its processor time (`clock()`) where it
 * reads no clock of the elapsed time.  It reads the tick count or the
 * performance counter on Windows, and `clock_gettime()` with
 * `HAVE_CLOCK_GETTIME`, which `include/private/gcconfig.h` also defines on
 * Darwin, Cosmopolitan and Haiku, and with `_POSIX_TIMERS` on Cygwin and on
 * Linux with glibc (`__USE_POSIX199309`).
 */
#if defined(BSD_TIME)                                       \
    || !(defined(_WIN32) || defined(HAVE_CLOCK_GETTIME)     \
         || (defined(__APPLE__) && defined(__MACH__))       \
         || defined(__COSMOPOLITAN__) || defined(__HAIKU__) \
         || (defined(_POSIX_TIMERS)                         \
             && (defined(__CYGWIN__)                        \
                 || (defined(__linux__) && defined(__USE_POSIX199309)))))
#  define PROCESSOR_TIME_LIMIT
#endif

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

/* The time in microseconds, by the clock of the collector. */
static double
now_us(void)
{
#  ifdef BSD_TIME
  struct rusage ru;

  (void)getrusage(RUSAGE_SELF, &ru);
  return (double)ru.ru_utime.tv_sec * 1e6 + (double)ru.ru_utime.tv_usec;
#  elif defined(PROCESSOR_TIME_LIMIT)
  return (double)clock() * 1e6 / CLOCKS_PER_SEC;
#  else
  struct timespec ts;

  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
#  endif
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
new_node(GC_word value)
{
  struct node *n = (struct node *)GC_MALLOC(sizeof(struct node));

  CHECK(n != NULL);
  n->value = value;
  return n;
}

/* Build a complete tree of `n` nodes, of the values from `first` on. */
static struct node *
build(GC_word first, GC_word n)
{
  struct node *t;
  GC_word left_n;

  if (0 == n)
    return NULL;
  t = new_node(first);
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

/*
 * Allocate short-lived objects until `n` more collections completed.
 * Return the most bytes the heap had in use meanwhile.
 */
static size_t
churn(GC_word n)
{
  GC_word start = GC_get_gc_no();
  size_t max_in_use = 0;
  unsigned long i;

  for (i = 0; GC_get_gc_no() - start < n; i++) {
    struct node *p = new_node((GC_word)i);
    size_t in_use;

    /* Replace a leaf of the live tree now and then, with a barrier. */
    if (i % 64 == 0) {
      struct node *t = root;

      while (t->left != NULL && t->left->left != NULL)
        t = (i >> 6) % 2 != 0 ? t->left : t->right;
      p->value = t->value;
      GC_PTR_STORE_AND_DIRTY(&t->left, p);
    }
    in_use = GC_get_heap_size() - GC_get_free_bytes();
    if (in_use > max_in_use)
      max_in_use = in_use;
  }
  return max_in_use;
}

/*
 * The callers that hold the allocator lock until the marking in progress
 * completes get it at once.
 */
static void
test_awaited_marking(void)
{
  int first;

  n_stops = 0;
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
}

/*
 * The client keeps at least half of the time.  An allocation budget would
 * bound the heap cap to the live data plus the budget percentage of it,
 * which the bytes in use may approach while a collection marks.
 */
static void
test_low_pressure(void)
{
  GC_word budget = GC_get_alloc_budget_percent();
  int i, n_pairs = 0, n_short = 0;

  GC_set_alloc_budget_percent(0);
  n_stops = 0;
  GC_set_on_collection_event(on_event);
  (void)churn(N_COLLECTIONS);
  GC_set_on_collection_event(0);
  GC_set_alloc_budget_percent(budget);
  for (i = 0; i + 1 < n_stops; i++) {
    double stopped = stop_end[i] - stop_start[i];
    double running = stop_start[i + 1] - stop_end[i];

    /* The first mark of a collection is due by the allocation alone. */
    if (completes[i])
      continue;
    n_pairs++;
    if (running + 50 < 0.9 * stopped) {
      if (n_short < 5)
        fprintf(stderr, "Stop %d of %.0f us followed by %.0f us running\n", i,
                stopped, running);
      n_short++;
    }
  }
  printf("Low pressure: %d world stops, %d paced, %d followed by a shorter"
         " run\n",
         n_stops, n_pairs, n_short);
  CHECK(n_pairs >= N_COLLECTIONS);
  CHECK(0 == n_short);
}

/*
 * The memory limit puts the heap ceiling (75% of it, by default) at 1.875
 * times the bytes in use and the heap cap halfway between them.
 */
static void
test_high_pressure(void)
{
  struct GC_timeval_s tv;
  size_t max_in_use, limit, ceiling;

  GC_gcollect_and_unmap();
  limit = 5 * (GC_get_heap_size() - GC_get_free_bytes()) / 2;
  printf("Heap %lu MiB mapped, %lu MiB free\n",
         (unsigned long)(GC_get_heap_size() / MiB),
         (unsigned long)(GC_get_free_bytes() / MiB));
  GC_set_memory_limit(limit);
  ceiling = limit / 100 * GC_get_heap_ceiling_percent();
  tv.tv_ms = 0;
  tv.tv_nsec = 10 * 1000;
  GC_set_time_limit_tv(tv);
  max_in_use = churn(N_COLLECTIONS);
  printf("High pressure: memory limit %lu MiB, heap ceiling %lu MiB, most"
         " in use %lu MiB\n",
         (unsigned long)(limit / MiB), (unsigned long)(ceiling / MiB),
         (unsigned long)(max_in_use / MiB));
  CHECK(max_in_use <= ceiling);
}

int
main(void)
{
  int full_freq;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  GC_enable_incremental();
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The incremental mode with the manual VDB is unavailable,"
           " test skipped\n");
    return 0;
  }
  GC_set_time_limit(TIME_LIMIT_MS);
  full_freq = GC_get_full_freq();
  GC_set_full_freq(0);
  root = build(0, N_NODES);
  GC_gcollect();
  test_awaited_marking();
  if (0 == GC_get_memory_limit()) {
    printf("The memory limit is unknown, the pacing is not tested\n");
  } else {
    test_low_pressure();
    GC_set_full_freq(full_freq);
    test_high_pressure();
  }
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The parallel marker, the clock or the incremental mode is"
         " unavailable, or the clock counts milliseconds, test skipped\n");
  return 0;
}
#endif
