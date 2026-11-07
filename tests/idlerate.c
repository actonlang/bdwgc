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
 * Test that the allocation rate that the collector measures for the pacing
 * of the steps (see `prev_allocd`) does not count the time for which the
 * client idles between collections: the incremental mode with the manual
 * VDB, an allocation budget of 250%, and no full collection by the count of
 * minor ones or by the growth of the bytes in use after them
 * (`GC_GEN_FULL_LIVE_PCT`), so that only the heap cap makes a collection
 * full.  The time limit is a tenth of what the marking of a full collection
 * of the live tree took (at least 100 microseconds), so that it scales with
 * the speed of the build, and a refill of a thread-local free list takes one
 * block, as the steps run only at the calls of the collector from the
 * allocator, at most one per refill.  The client keeps a live tree and a ring
 * of its latest objects, and allocates without pause until `N_STEADY` minor
 * collections completed.
 *
 * Then the client idles after a finish that leaves no full collection due
 * (`GC_need_full_gc`), for 3 times the average length of a cycle of the first
 * part, and allocates as before, 3 times.  The median of the allocation rates
 * that the collector measures at the end of the markings after the idle
 * periods must be at least half the median of those it measured for the
 * minor collections of the first part, so that the headroom that the pacing
 * keeps for the marking does not shrink after an idle period.  The checks
 * of the first part count only its own collections.
 *
 * The two parts run up to `MAX_RUNS` times in all, again only while a run
 * fails this check: on a loaded machine the allocation rate varies from run
 * to run, and a run can measure the rates after the idle periods under more
 * load than those before them, while with a window of the allocation rate
 * that starts at the finish of the previous collection, every run fails.
 * Where the clock of the collector reads the processor time of the process
 * (`clock()` where the collector reads no clock of the elapsed time, and the
 * user time with `BSD_TIME`), it does not advance while the client idles,
 * and the check holds with any window.
 *
 * This test includes the collector source to read the pacing state.
 * Without the manual VDB mode, the parallel marker, the clock or a known
 * memory limit, if `NO_INCREMENTAL` is defined, or on Windows, where the
 * clock of the collector counts milliseconds, the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define GC_SINGLE_OBJ_BUILD
#ifndef __cplusplus
#  define GC_INNER STATIC
#  define GC_EXTERN GC_INNER
#endif
#undef GC_PTHREAD_START_STANDALONE
#undef GC_DLL
#ifndef GC_NOT_DLL
#  define GC_NOT_DLL
#endif
#include "private/gc_priv.h"

#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
/* See the same in `memlimit.c` file. */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
#include "../extra/gc.c"
#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
#  pragma GCC diagnostic pop
#endif

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#if defined(PARALLEL_MARK_DEADLINE) && !defined(NO_INCREMENTAL) \
    && !defined(_WIN32)

/* The nodes of the live tree, of 4 words each (8 MiB). */
#  define N_NODES (1 << 18)

/* The objects in the ring (2 MiB of nodes). */
#  define N_RING (1 << 16)

/*
 * The minor collections to run, and the collections to run at most, in
 * a run of the first part, and the runs of both parts at most.
 */
#  define N_STEADY 24
#  define MAX_COLLECTIONS 200
#  define MAX_RUNS 3

/*
 * The slices of a full marking that the time limit is set for, and the
 * least time limit, in microseconds.
 */
#  define FULL_MARK_SLICES 10
#  define MIN_TIME_LIMIT_US 100

/* The idle periods of the last part. */
#  define N_IDLE 3

struct node {
  struct node *left;
  struct node *right;
  GC_word value;
  GC_word pad;
};

static struct node *volatile root;
static struct node **volatile ring;

/*
 * The collections started and marked, and the full ones among the latter.
 * Protected by the allocator lock.
 */
static unsigned long n_started, n_marked, n_full;

/*
 * Whether the latest collection marked was full.  The allocation rates in
 * bytes per millisecond that the collector measured for the minor
 * collections of the first part, and their count.  In the last part,
 * whether an idle period is due, whether the client idled since the latest
 * finish, and the rates measured for the collections after an idle period,
 * and their count.  Protected by the allocator lock.
 */
static int marked_full;
static word steady_rates[MAX_COLLECTIONS];
static unsigned long n_steady_rates;
static unsigned long idle_due, idled;
static word idle_rates[N_IDLE];
static unsigned long n_idle_rates;

/*
 * The allocation rate that the collector measured at the end of the
 * latest marking, in bytes per millisecond, or 0 if unknown.
 */
static word
measured_rate(void)
{
  return prev_client_us > 0 ? scale_word(prev_allocd, 1000, prev_client_us)
                            : 0;
}

/*
 * The collection events of the first part.  Called with the allocator lock
 * held.
 */
static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_PRE_STOP_WORLD == e && 0 == n_slices) {
    /* The first mark of a collection. */
    n_started++;
  } else if (GC_EVENT_MARK_END == e) {
    n_marked++;
    marked_full = GC_is_full_gc;
    if (GC_is_full_gc)
      n_full++;
  } else if (GC_EVENT_RECLAIM_END == e) {
    if (!marked_full && n_steady_rates < MAX_COLLECTIONS)
      steady_rates[n_steady_rates++] = measured_rate();
  }
}

/*
 * The collection events of the last part, which counts the collections
 * started but leaves the counts and the measures of the first part as they
 * were at its end, for the checks after the runs.  Called with the
 * allocator lock held.
 */
static void GC_CALLBACK
on_idle_part_event(GC_EventType e)
{
  if (GC_EVENT_PRE_STOP_WORLD == e && 0 == n_slices) {
    n_started++;
  } else if (GC_EVENT_RECLAIM_END == e) {
    if (idled && n_idle_rates < N_IDLE) {
      idle_rates[n_idle_rates++] = measured_rate();
      idled = 0;
    }
    if (!GC_need_full_gc)
      idle_due = 1;
  }
}

/* Sort the `n` values from `a` on in ascending order. */
static void
sort_words(word *a, unsigned long n)
{
  unsigned long i, j;

  for (i = 1; i < n; i++) {
    word v = a[i];

    for (j = i; j > 0 && a[j - 1] > v; j--)
      a[j] = a[j - 1];
    a[j] = v;
  }
}

static void
sleep_ms(unsigned long ms)
{
  struct timespec ts;

  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
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

static unsigned long
locked_count(const unsigned long *p)
{
  unsigned long n;

  LOCK();
  n = *p;
  UNLOCK();
  return n;
}

int
main(void)
{
  struct GC_timeval_s tv;
  unsigned long i, j, us, idle_ms, first_part_us, max_started;
  CLOCK_TYPE start;
  word median = 0;
  int run, passed;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  GC_enable_incremental();
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  if (0 == GC_get_memory_limit()) {
    printf("The memory limit is unknown, test skipped\n");
    return 0;
  }
  tv.tv_ms = 1;
  tv.tv_nsec = 0;
  GC_set_time_limit_tv(tv);
  GC_set_alloc_budget_percent(250);
  GC_set_full_freq(MAX_COLLECTIONS);
  GC_set_tl_refill_max(1);
  LOCK();
  GC_gen_full_live_pct = -1;
  UNLOCK();
  root = build(0, N_NODES);
  ring = (struct node **)GC_MALLOC(N_RING * sizeof(struct node *));
  CHECK(ring != NULL);
  GC_gcollect();
  LOCK();
  us = prev_mark_us[1] / FULL_MARK_SLICES;
  UNLOCK();
  if (us < MIN_TIME_LIMIT_US)
    us = MIN_TIME_LIMIT_US;
  tv.tv_ms = us / 1000;
  tv.tv_nsec = us % 1000 * 1000;
  GC_set_time_limit_tv(tv);
  i = 0;
  for (run = 1;; run++) {
    LOCK();
    n_started = n_marked = n_full = 0;
    n_steady_rates = 0;
    idle_due = idled = n_idle_rates = 0;
    UNLOCK();
    GC_set_on_collection_event(on_event);
    GET_TIME(start);
    for (;; i++) {
      GC_PTR_STORE_AND_DIRTY(&ring[i % N_RING], new_node((GC_word)i));
      if (i % 1024 != 0)
        continue;
      if (locked_count(&n_steady_rates) >= N_STEADY
          || locked_count(&n_started) >= MAX_COLLECTIONS)
        break;
    }
    first_part_us = us_since(start);
    GC_set_on_collection_event(0);
    printf("Run %d: time limit %lu us; %lu collections, %lu full\n", run, us,
           n_marked, n_full);
    passed = n_steady_rates > 0;
    if (passed) {
      sort_words(steady_rates, n_steady_rates);
      median = steady_rates[n_steady_rates / 2];
      idle_ms = 3 * first_part_us / 1000 / n_marked;
      if (idle_ms < 10)
        idle_ms = 10;
      LOCK();
      max_started = n_started + MAX_COLLECTIONS;
      UNLOCK();
      GC_set_on_collection_event(on_idle_part_event);
      for (;; i++) {
        GC_PTR_STORE_AND_DIRTY(&ring[i % N_RING], new_node((GC_word)i));
        if (i % 1024 != 0)
          continue;
        if (locked_count(&idle_due) != 0) {
          LOCK();
          idle_due = 0;
          idled = 1;
          UNLOCK();
          sleep_ms(idle_ms);
        }
        if (locked_count(&n_idle_rates) >= N_IDLE
            || locked_count(&n_started) >= max_started)
          break;
      }
      GC_set_on_collection_event(0);
      printf("Idle periods of %lu ms, rates after them", idle_ms);
      for (j = 0; j < n_idle_rates; j++)
        printf(" %lu", (unsigned long)idle_rates[j]);
      printf(" B/ms, median rate of %lu minor collections without them %lu"
             " B/ms\n",
             n_steady_rates, (unsigned long)median);
      sort_words(idle_rates, n_idle_rates);
      passed = median > 0 && n_idle_rates >= N_IDLE
               && idle_rates[N_IDLE / 2] >= median / 2;
    }
    if (passed || run >= MAX_RUNS)
      break;
  }
  CHECK(n_steady_rates > 0);
  CHECK(median > 0);
  CHECK(n_idle_rates >= N_IDLE);
  CHECK(idle_rates[N_IDLE / 2] >= median / 2);
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
