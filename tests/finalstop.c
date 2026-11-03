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
 * Test the time limit of the final world-stopped marks of a collection:
 * the incremental mode with the manual VDB and a live list of about 1 MiB.
 * Each part of the test runs `N_COLLECTIONS` collections, which must
 * complete before the heap grows beyond `MAX_HEAP`, and counts the
 * world-stopped marks of each.
 *
 * First, with a time limit of 1 ms and a client stop function that returns
 * true during every world-stopped mark, the world-stopped marks of
 * a collection are abandoned after their roots are pushed, however long
 * their time limit, until a final mark runs without the time limit, and
 * steps mark in between.  Each collection takes 5 marks: the first one, the
 * 3 final ones abandoned before one runs without the time limit (whatever
 * `GC_get_max_prior_attempts()` is, which the test sets to 1), and that
 * one.  The stop function lets a step mark until its deadline: with one
 * `GC_mark_some()` per step, the steps would fall behind the allocation in
 * a program with megabytes of static data, e.g. one linked with
 * a sanitizer runtime, whose roots every world-stopped mark pushes again.
 *
 * Second, with a time limit of `LIMIT_MS`, the stop function stops only the
 * first mark of each collection, and the test sleeps after every step for
 * twice the time limit.  A final mark, which scans a large object that only
 * a root holds since the first mark, gets the whole time limit from its own
 * start, also with `GC_get_max_prior_attempts()` at 0: it completes in time,
 * or (e.g. on a loaded machine) it is abandoned once the time limit has
 * passed, not earlier (see `note_abandoned_final()`).  Each collection takes
 * 2 marks, and one more for each final mark that was abandoned.
 *
 * Third, with the time unlimited and every collection full, the stop
 * function of the first part leaves the rule to
 * `GC_get_max_prior_attempts()` (1): each collection takes 3 marks.  It lets
 * a step mark to the end of the marking: without thread-local allocation,
 * the steps run only before the heap grows.
 *
 * Without the steps (e.g. without the parallel marker), the manual VDB mode
 * abandons no mark, and every collection completes in its first one.
 * Without the manual VDB mode, or if `NO_INCREMENTAL` is defined, the test
 * is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
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

/* The nodes of the live list, of 4 words each. */
#define N_NODES (32 * 1024)

/* The collections of a part, and the heap size not to exceed meanwhile. */
#define N_COLLECTIONS 5
#define MAX_HEAP (512 * MiB)

/*
 * Defined where the collector counts its time limit in processor time (see
 * `GET_TIME()` in `include/private/gc_priv.h`): in the user time of the
 * process with `BSD_TIME`, and in its processor time (`clock()`) where it
 * reads no clock of the elapsed time.  It reads the tick count or the
 * performance counter on Windows, and `clock_gettime()` with
 * `HAVE_CLOCK_GETTIME`, which `include/private/gcconfig.h` also defines on
 * Cosmopolitan and Haiku, and with `_POSIX_TIMERS` on Cygwin and on Linux
 * with glibc (`__USE_POSIX199309`).
 */
#if defined(BSD_TIME)                                       \
    || !(defined(_WIN32) || defined(HAVE_CLOCK_GETTIME)     \
         || defined(__COSMOPOLITAN__) || defined(__HAIKU__) \
         || (defined(_POSIX_TIMERS)                         \
             && (defined(__CYGWIN__)                        \
                 || (defined(__linux__) && defined(__USE_POSIX199309)))))
#  define PROCESSOR_TIME_LIMIT
#endif

/*
 * The time limit of the second part, and how much less than it the test may
 * see a final mark run that is abandoned (see `note_abandoned_final()`), in
 * milliseconds.  On Windows the clock of the test, the tick count
 * (`GetTickCount64()`, or `GetTickCount()` for targets before Windows
 * Vista), advances every 10 to 16 ms, while the collector may read the
 * performance counter.  The processor time that the test reads with
 * `PROCESSOR_TIME_LIMIT` misses the few microseconds from the read of the
 * collector to the latest stop of the world.
 */
#define LIMIT_MS 100
#ifdef _WIN32
#  define SLACK_MS 16
#else
#  define SLACK_MS 1
#endif

struct node {
  struct node *next;
  GC_word value;
  GC_word pad[2];
};

static struct node *volatile live;
static void *volatile junk;
static void *volatile big;

/*
 * The part that runs (0 if none), whether the world was stopped for
 * a world-stopped mark (not for a step), the world-stopped marks of the
 * collection in progress, and those of each collection of the part that
 * completed.  Updated by `on_collection_event()`, which the collector calls
 * holding the allocator lock on the thread that collects.
 */
static volatile int part;
static volatile int marking;
static volatile unsigned n_marks;
static volatile unsigned n_counted;
static unsigned marks_of[N_COLLECTIONS];

/*
 * For the second part (see `note_abandoned_final()`): when the allocation
 * in progress started, or when the latest world stop ended if that was
 * later (by `now_us()`), the abandoned final marks of each collection and
 * of the part, and the first final mark abandoned before the time limit had
 * passed: its collection (counted from 1, or 0 if none) and how long it
 * ran.  With `PROCESSOR_TIME_LIMIT`, also the processor time at the latest
 * stop of the world, and how long that final mark ran on it.
 */
static volatile unsigned long since_us;
static unsigned abandoned_of[N_COLLECTIONS];
static unsigned n_abandoned;
static unsigned early_in;
static unsigned long early_us;
#ifdef PROCESSOR_TIME_LIMIT
static volatile clock_t stop_clock;
static unsigned long early_cpu_us;
#endif

static void
sleep_ms(unsigned ms)
{
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts;

  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
#endif
}

/*
 * The time in microseconds, by the tick count on Windows and by the
 * monotonic clock elsewhere, wrapping around (only the differences of the
 * values are used).
 */
static unsigned long
now_us(void)
{
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
  return (unsigned long)(GetTickCount64() * 1000U);
#  else
  return (unsigned long)GetTickCount() * 1000UL;
#  endif
#else
  struct timespec ts;

  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000000UL
         + (unsigned long)(ts.tv_nsec / 1000);
#endif
}

/*
 * Count a final mark of the second part that was abandoned, the world
 * still stopped.  The collector counts the time limit from a read of its
 * clock just before it stops the world (see `PROCESSOR_TIME_LIMIT`).  The
 * final mark must have run for the time limit, less `SLACK_MS`, by
 * `now_us()` since the allocation that ran it started (or since the world
 * stop before ended, if later), as the thread may be preempted between
 * that read and the stop of the world.  With `PROCESSOR_TIME_LIMIT`, it may
 * instead have run for as long on the processor time since the latest stop
 * of the world, which a preempted thread does not advance, and which the
 * parallel markers may advance faster than the elapsed time passes.
 * Without it, the processor time does not count: there the parallel
 * markers may make it pass the time limit before the elapsed time does.
 */
static void
note_abandoned_final(void)
{
  unsigned long us = now_us() - since_us;

  if (n_counted < N_COLLECTIONS)
    abandoned_of[n_counted]++;
  n_abandoned++;
  if (early_in != 0 || us >= (LIMIT_MS - SLACK_MS) * 1000UL)
    return;
#ifdef PROCESSOR_TIME_LIMIT
  early_cpu_us
      = (unsigned long)((double)(clock() - stop_clock) * 1e6 / CLOCKS_PER_SEC);
  if (early_cpu_us >= (LIMIT_MS - SLACK_MS) * 1000UL)
    return;
#endif
  early_in = n_counted + 1;
  early_us = us;
}

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  if (0 == part)
    return;
  switch (e) {
#ifdef PROCESSOR_TIME_LIMIT
  case GC_EVENT_PRE_STOP_WORLD:
    stop_clock = clock();
    break;
#endif
  case GC_EVENT_POST_STOP_WORLD:
    marking = 0;
    break;
  case GC_EVENT_MARK_START:
    marking = 1;
    n_marks++;
    break;
  case GC_EVENT_MARK_END:
    if (n_counted < N_COLLECTIONS)
      marks_of[n_counted] = n_marks;
    n_counted++;
    n_marks = 0;
    break;
  case GC_EVENT_PRE_START_WORLD:
    /* A final mark that was abandoned: one that completed reset `n_marks`. */
    if (2 == part && marking && n_marks > 1)
      note_abandoned_final();
    break;
  case GC_EVENT_POST_START_WORLD:
    /* A final mark that follows the step starts long after the step. */
    if (2 == part && !marking)
      sleep_ms(2 * LIMIT_MS);
    marking = 0;
    since_us = now_us();
    break;
  default:
    break;
  }
}

static int GC_CALLBACK
stop_first(void)
{
  return marking && 1 == n_marks;
}

static int GC_CALLBACK
stop_marks(void)
{
  return marking;
}

/*
 * Run part `p`: complete `N_COLLECTIONS` collections, each of which must
 * take `marks` world-stopped marks, or as many as the first one if `marks`
 * is 0, and in the second part one more for each of its final marks that
 * was abandoned.  Return the marks of the first collection.
 */
static unsigned
run_part(int p, unsigned marks)
{
  GC_word start;
  unsigned long i;
  unsigned k, big_for = (unsigned)-1;

  n_marks = 0;
  n_counted = 0;
  for (k = 0; k < N_COLLECTIONS; k++)
    abandoned_of[k] = 0;
  part = p;
  start = GC_get_gc_no();
  for (i = 0; GC_get_gc_no() - start < N_COLLECTIONS; i++) {
    if (2 == p)
      since_us = now_us();
    junk = GC_MALLOC(sizeof(struct node));
    CHECK(junk != NULL);
    if (2 == p && 1 == n_marks && big_for != n_counted) {
      /* The first mark was abandoned: only a root holds the new object. */
      since_us = now_us();
      big = GC_MALLOC(MiB);
      CHECK(big != NULL);
      big_for = n_counted;
    }
    if (i % 1024 == 0 && GC_get_heap_size() > MAX_HEAP) {
      fprintf(stderr, "Heap %lu MiB after %lu collections\n",
              (unsigned long)(GC_get_heap_size() / MiB),
              (unsigned long)(GC_get_gc_no() - start));
      CHECK(0);
    }
  }
  part = 0;
  CHECK(n_counted >= N_COLLECTIONS);
  if (early_in != 0) {
#ifdef PROCESSOR_TIME_LIMIT
    fprintf(stderr,
            "Part %d: a final mark of collection %u was abandoned after %lu us"
            " (%lu us of processor time), before the time limit of %d ms\n",
            p, early_in, early_us, early_cpu_us, LIMIT_MS);
#else
    fprintf(stderr,
            "Part %d: a final mark of collection %u was abandoned after %lu us"
            ", before the time limit of %d ms\n",
            p, early_in, early_us, LIMIT_MS);
#endif
    exit(1);
  }
  if (0 == marks)
    marks = marks_of[0];
  for (k = 0; k < N_COLLECTIONS; k++) {
    if (marks_of[k] != marks + abandoned_of[k]) {
      fprintf(stderr,
              "Part %d: collection %u took %u world-stopped marks, not %u\n",
              p, k + 1, marks_of[k], marks + abandoned_of[k]);
      exit(1);
    }
  }
  return marks_of[0];
}

int
main(void)
{
  GC_stop_func old_stop_func;
  unsigned long i;
  unsigned first;
  int steps;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
#ifndef NO_INCREMENTAL
  GC_enable_incremental();
#endif
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The incremental mode with the manual VDB is unavailable,"
           " test skipped\n");
    return 0;
  }
  for (i = 0; i < N_NODES; i++) {
    struct node *n = (struct node *)GC_MALLOC(sizeof(struct node));

    CHECK(n != NULL);
    n->value = (GC_word)i;
    GC_PTR_STORE_AND_DIRTY(&n->next, live);
    live = n;
  }
  GC_gcollect();
  GC_set_on_collection_event(on_collection_event);
  old_stop_func = GC_get_stop_func();

  GC_set_time_limit(1);
  GC_set_max_prior_attempts(1);
  GC_set_stop_func(stop_marks);
  first = run_part(1, 0);
  steps = first != 1;
  if (steps && first != 5) {
    fprintf(stderr,
            "Part 1: collection 1 took %u world-stopped marks, not 5\n",
            first);
    exit(1);
  }

  GC_set_time_limit(LIMIT_MS);
  GC_set_max_prior_attempts(0);
  GC_set_stop_func(stop_first);
  (void)run_part(2, steps ? 2 : 1);

  GC_set_time_limit(GC_TIME_UNLIMITED);
  GC_set_max_prior_attempts(1);
  GC_set_full_freq(0);
  GC_set_stop_func(stop_marks);
  (void)run_part(3, steps ? 3 : 1);

  GC_set_stop_func(old_stop_func);
  printf("Collections in %u, %u and %u world-stopped marks, %u more for"
         " final marks abandoned in the second part, heap %lu MiB\n",
         first, steps ? 2U : 1U, steps ? 3U : 1U, n_abandoned,
         (unsigned long)(GC_get_heap_size() / MiB));
  printf("SUCCEEDED\n");
  return 0;
}
