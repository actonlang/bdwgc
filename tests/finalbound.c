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
 * the incremental mode with the manual VDB.  This test includes the
 * collector source to read the time limit of each final mark.
 *
 * The first four parts keep a live list of about 1 MiB.  Each runs
 * `N_COLLECTIONS` collections, which must complete before the heap grows
 * beyond `MAX_HEAP`, and counts the world-stopped marks of each.  The
 * first three parts run without the heap cap
 * (`GC_set_heap_ceiling_percent(0)`), thus without the pacing of the steps
 * by the memory headroom.
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
 * the steps run only before the heap grows, except on Windows.  There the
 * client calls `GC_collect_a_little()` once every 1024 allocations, as every
 * request of memory from the system takes one of the `MAX_HEAP_SECTS` slots,
 * 512 in a 32-bit build without `LARGE_CONFIG` and `SMALL_CONFIG`, the scratch
 * chunks for the block headers included, and the heap growth after each slice
 * that leaves the marking unfinished, 4 per collection, can use them all up.
 *
 * Fourth, as the first part but with the heap cap, the time limit of a final
 * mark grows with each one abandoned, also by the stop function of the
 * client, up to 4 times the time limit, where the stop function no longer
 * stops it: each collection takes 2 to 4 marks (the first one, at most two
 * final marks abandoned below that bound, and one at it), and one more for
 * each final mark abandoned at the bound, by its time limit alone (on
 * a loaded machine, or in a slow build, the part before the roots are pushed
 * may take longer than the bound), of which there are
 * `MAX_ABANDONED_AT_BOUND` at most.  The part fails if the stop function of
 * the client is called during a final mark at the bound.  Without a known
 * memory limit, or with a heap ceiling of 0, there is no heap cap, and each
 * collection takes 5 marks, as in the first part.
 *
 * Last, with a time limit of 1 ms, no allocation budget (so that the heap cap
 * is the live data plus half of the distance from it to the heap ceiling) and
 * a heap ceiling of at least `MIN_CEILING_MIB` MiB, so that the heap cap is
 * far above the heap of the test whatever the memory limit, the client links a
 * new list of `N_LIST` nodes to a root, allocated with the collections
 * disabled, before a collection (so that its first mark does not complete),
 * once the collection marks in steps, and again after every abandoned final
 * mark, until a final mark at `MAX_SLICE_LIMITS` times the time limit (the
 * bound) is abandoned.  The steps do not mark from the roots, thus every final
 * mark has the latest list to mark, which takes longer than the bound, and the
 * need of the next final mark doubles with each one abandoned.  The part runs
 * three times.  In each, a final mark may run without a time limit outside one
 * stop only after `MAX_ABANDONED_AT_BOUND` final marks abandoned at the bound,
 * or with nothing allocated since the latest of them.  At most that many final
 * marks may be abandoned at the bound, and the collection must complete after
 * the first one abandoned there, not in one stop.  First, the client goes on
 * allocating after that.  The final marks after the first one abandoned at
 * the bound complete at it if the part before their roots are pushed (the
 * world stop, the dirty bits, the walk of the blocks for the dirty ones, the
 * roots) leaves room.  Otherwise the final mark after
 * `MAX_ABANDONED_AT_BOUND` abandoned at the bound runs without a time limit,
 * as in a zig build in the Debug mode, where that part takes longer than the
 * bound with a heap of a few hundred MiB.  Second, the client links a new list
 * after every final mark abandoned, also at the bound, and calls
 * `GC_collect_a_little()` in between, so that no final mark at the bound
 * completes (a list is marked by one marker, for longer than the bound):
 * exactly one final mark must run without a time limit, after exactly
 * `MAX_ABANDONED_AT_BOUND` abandoned at the bound.  Third, the client waits
 * for the collection by calling `GC_collect_a_little()` until it returns 0,
 * with nothing allocated: the wait must end, and at most one more final mark
 * may be abandoned at the bound.
 *
 * Without the steps (e.g. without the parallel marker), the manual VDB mode
 * abandons no mark, and every collection completes in its first one.
 * Without the manual VDB mode, or if `NO_INCREMENTAL` is defined, the test
 * is skipped.  The last part is skipped without the parallel marker, the
 * clock or a known memory limit, and on Windows, where the clock of the
 * collector counts milliseconds.
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
#  ifndef NO_CLOCK
#    define PROCESSOR_TIME_LIMIT
#  endif
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
 * a world-stopped mark (not for a step), whether a step of the second part
 * ended since the client last slept, the world-stopped marks of the
 * collection in progress, and those of each collection of the part that
 * completed.  Updated by `on_collection_event()`, which the collector calls
 * holding the allocator lock on the thread that collects.
 */
static volatile int part;
static volatile int marking;
static volatile int step_ended;
static volatile unsigned n_marks;
static volatile unsigned n_counted;
static unsigned marks_of[N_COLLECTIONS];

/*
 * For the second part (see `note_abandoned_final()`): when the allocation in
 * progress started, or when the latest world stop ended if that was later (by
 * `now_us()`), the abandoned final marks of each collection (in the fourth
 * part, those abandoned at the bound) and of the part, and the first final
 * mark abandoned before the time limit had passed: its collection (counted
 * from 1, or 0 if none) and how long it ran.  With `PROCESSOR_TIME_LIMIT`,
 * also the processor time at the latest stop of the world, by the clock of the
 * collector, and how long that final mark ran on it.
 */
static volatile unsigned long since_us;
static unsigned abandoned_of[N_COLLECTIONS];
static unsigned n_abandoned;
static unsigned early_in;
static unsigned long early_us;
#ifdef PROCESSOR_TIME_LIMIT
static CLOCK_TYPE stop_clock;
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
#ifdef PROCESSOR_TIME_LIMIT
  CLOCK_TYPE now;
#endif

  if (n_counted < N_COLLECTIONS)
    abandoned_of[n_counted]++;
  n_abandoned++;
  if (early_in != 0 || us >= (LIMIT_MS - SLACK_MS) * 1000UL)
    return;
#ifdef PROCESSOR_TIME_LIMIT
  GET_TIME(now);
  early_cpu_us = MS_TIME_DIFF(now, stop_clock) * 1000UL
                 + NS_FRAC_TIME_DIFF(now, stop_clock) / 1000UL;
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
    GET_TIME(stop_clock);
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
    if (marking && n_marks > 1) {
      if (2 == part)
        note_abandoned_final();
#ifdef PARALLEL_MARK_DEADLINE
      else if (4 == part && final_mark_bounded && n_counted < N_COLLECTIONS)
        abandoned_of[n_counted]++;
#endif
    }
    break;
  case GC_EVENT_POST_START_WORLD:
    if (2 == part && !marking)
      step_ended = 1;
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

/*
 * The calls of `stop_marks()` during a final mark at the bound, which the
 * stop function of the client must not stop.  Updated holding the lock.
 */
static unsigned long n_stops_at_bound;

static int GC_CALLBACK
stop_marks(void)
{
#if !defined(GC_DISABLE_INCREMENTAL) && !defined(NO_CLOCK)
  if (final_mark_bounded)
    n_stops_at_bound++;
#endif
  return marking;
}

/*
 * Run part `p`: complete `N_COLLECTIONS` collections, each of which must
 * take `marks` to `max_marks` world-stopped marks, or as many as the first
 * one if `marks` is 0, and one more for each of its final marks that was
 * abandoned in the second part, or at the bound in the fourth one.  Return
 * the marks of the first collection.
 */
static unsigned
run_part(int p, unsigned marks, unsigned max_marks)
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
    if (step_ended) {
      /*
       * A final mark that follows the step starts long after the step.
       * The client sleeps outside the collector: a sleep in the event
       * callback would count as part of the step, and the world would run
       * as long again before the final mark, with the client allocating.
       */
      step_ended = 0;
      sleep_ms(2 * LIMIT_MS);
    }
    if (2 == p && 1 == n_marks && big_for != n_counted) {
      /* The first mark was abandoned: only a root holds the new object. */
      since_us = now_us();
      big = GC_MALLOC(MiB);
      CHECK(big != NULL);
      big_for = n_counted;
    }
#ifdef _WIN32
    if (3 == p && i % 1024 == 0) {
      /* See the third part at the top of this file. */
      (void)GC_collect_a_little();
    }
#endif
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
    marks = max_marks = marks_of[0];
  for (k = 0; k < N_COLLECTIONS; k++) {
#ifdef PARALLEL_MARK_DEADLINE
    if (4 == p && abandoned_of[k] > MAX_ABANDONED_AT_BOUND) {
      fprintf(stderr,
              "Part 4: collection %u had %u final marks abandoned at the"
              " bound, more than %d\n",
              k + 1, abandoned_of[k], MAX_ABANDONED_AT_BOUND);
      exit(1);
    }
#endif
    if (marks_of[k] < marks + abandoned_of[k]
        || marks_of[k] > max_marks + abandoned_of[k]) {
      fprintf(stderr,
              "Part %d: collection %u took %u world-stopped marks, not %u"
              " to %u\n",
              p, k + 1, marks_of[k], marks + abandoned_of[k],
              max_marks + abandoned_of[k]);
      exit(1);
    }
  }
  return marks_of[0];
}

#if defined(PARALLEL_MARK_DEADLINE) && !defined(_WIN32)

/* The nodes of a list of the last part, of 2 words each. */
#  define N_LIST (1 << 21)

/* The collections to run at most in the last part. */
#  define MAX_COLLECTIONS 40

/*
 * The heap ceiling of the last part at least, in MiB.  The heap cap, the live
 * data plus half of the distance to the ceiling, is then at least about
 * 4 GiB, above the heap of the test whatever the memory limit: in a zig build
 * in the Debug mode that heap grows up to about 1.5 GiB, also at 16 KiB
 * blocks.  On 32-bit targets the ceiling saturates at the largest word.
 */
#  define MIN_CEILING_MIB 8192

struct list_node {
  struct list_node *next;
  GC_word value;
};

static struct list_node *volatile list;

/*
 * The final marks, those without a time limit outside one stop before
 * (`n_unlimited`) and after (`n_capped`) the final marks abandoned at the
 * bound let them run so (see `final_limit_us()`), those at the bound and
 * those of them abandoned, and all those abandoned; the collections
 * marked, and whether one completed its marking after a final mark at the
 * bound was abandoned (1, or 2 if in one stop).  Protected by the
 * allocator lock.
 */
static unsigned long n_finals, n_unlimited, n_capped, n_bound;
static unsigned long n_bound_abandoned, n_finals_abandoned, n_marked;
static int completed_after_bound;

/* The state of the final mark in progress. */
static GC_bool final_in_progress, final_at_bound, final_done;

/* Called with the allocator lock held. */
static void GC_CALLBACK
on_bound_event(GC_EventType e)
{
  if (GC_EVENT_PRE_STOP_WORLD == e && final_mark_no != 0) {
    n_finals++;
    final_in_progress = TRUE;
    final_done = FALSE;
    if (final_mark_unlimited && !in_one_stop) {
      if (n_final_abandoned_at_bound >= MAX_ABANDONED_AT_BOUND
          || (n_final_abandoned_at_bound > 0
              && allocd_now() == allocd_at_bound_abandon)) {
        n_capped++;
      } else {
        n_unlimited++;
      }
    }
    final_at_bound = !final_mark_unlimited
                     && slice_limit_us == MAX_SLICE_LIMITS * time_limit_us();
    if (final_at_bound)
      n_bound++;
  } else if (GC_EVENT_MARK_END == e) {
    n_marked++;
    final_done = TRUE;
    if (n_bound_abandoned > 0 && 0 == completed_after_bound)
      completed_after_bound = in_one_stop ? 2 : 1;
  } else if (GC_EVENT_POST_START_WORLD == e && final_in_progress) {
    final_in_progress = FALSE;
    if (!final_done) {
      n_finals_abandoned++;
      if (final_at_bound)
        n_bound_abandoned++;
    }
  }
}

/* Link a new list to the root, allocated with the collections disabled. */
static void
new_list(void)
{
  struct list_node *head = NULL;
  unsigned long i;

  GC_disable();
  for (i = 0; i < N_LIST; i++) {
    struct list_node *n
        = (struct list_node *)GC_MALLOC(sizeof(struct list_node));

    CHECK(n != NULL);
    n->value = (GC_word)i;
    GC_PTR_STORE_AND_DIRTY(&n->next, head);
    head = n;
  }
  list = head;
  GC_enable();
}

/* How the client runs after a final mark abandoned at the bound. */
#  define BOUND_ALLOC 0  /*< it allocates */
#  define BOUND_STEADY 1 /*< it links a new list after each abandoned */
#  define BOUND_WAIT 2   /*< it waits by calling `GC_collect_a_little()` */

/* The time the client waits at most in the last part, in seconds. */
#  define MAX_WAIT_S 60

/*
 * Wait for the collection in progress by calling `GC_collect_a_little()`
 * until it returns 0, with nothing allocated, for at most `MAX_WAIT_S`
 * seconds.
 */
static void
wait_for_collection(void)
{
  CLOCK_TYPE start;
  unsigned long calls;

  GET_TIME(start);
  for (calls = 1; GC_collect_a_little(); calls++) {
    if ((calls & 1023) == 0 && us_since(start) / 1000000UL >= MAX_WAIT_S) {
      fprintf(stderr,
              "GC_collect_a_little() still returns 1 after %d s"
              " (%lu calls)\n",
              MAX_WAIT_S, calls);
      CHECK(0);
    }
  }
}

/*
 * Run the last part, with the client running as `mode` says after the
 * first final mark abandoned at the bound.
 */
static void
run_bound_part(int mode)
{
  struct GC_timeval_s tv;
  unsigned long abandoned_at = 0, marked_at = 0;
  unsigned long bound_abandoned_at_wait = 0;
  GC_word list_gc_no = ~(GC_word)0;
  GC_bool after_bound = FALSE;
  /* The memory limit is known (see `main()`). */
  GC_word limit_mib = GC_get_memory_limit() / MiB;
  GC_word ceiling_percent
      = limit_mib > 0 ? MIN_CEILING_MIB * 100 / limit_mib + 1 : 0;

  n_finals = 0;
  n_unlimited = 0;
  n_capped = 0;
  n_bound = 0;
  n_bound_abandoned = 0;
  n_finals_abandoned = 0;
  n_marked = 0;
  completed_after_bound = 0;
  tv.tv_ms = 1;
  tv.tv_nsec = 0;
  GC_set_time_limit_tv(tv);
  GC_set_alloc_budget_percent(0);
  if (GC_get_heap_ceiling_percent() < ceiling_percent)
    GC_set_heap_ceiling_percent(ceiling_percent);
  GC_gcollect();
  GC_set_on_collection_event(on_bound_event);
  new_list();
  for (;;) {
    unsigned long abandoned, bound_abandoned, marked, unlimited;
    int completed;
    GC_bool in_progress;
    GC_word gc_no;

    /*
     * With `BOUND_STEADY`, the new lists are all the client allocates after
     * the first final mark abandoned at the bound, so that the heap does not
     * grow by the junk of the many steps after each abandoned final mark in
     * an unoptimized build.
     */
    if (after_bound) {
      (void)GC_collect_a_little();
    } else {
      junk = GC_MALLOC(sizeof(struct list_node));
      CHECK(junk != NULL);
    }
    LOCK();
    abandoned = n_finals_abandoned;
    bound_abandoned = n_bound_abandoned;
    marked = n_marked;
    unlimited = n_unlimited;
    completed = completed_after_bound;
    in_progress = GC_collection_in_progress() || FINAL_DUE();
    gc_no = GC_gc_no;
    UNLOCK();
    if (unlimited > 0) {
      fprintf(stderr, "Final mark without a time limit after %lu abandoned\n",
              abandoned);
      CHECK(0);
    }
    if (bound_abandoned > MAX_ABANDONED_AT_BOUND) {
      fprintf(stderr, "%lu final marks abandoned at the bound\n",
              bound_abandoned);
      CHECK(0);
    }
    if (completed != 0)
      break;
    if (marked >= MAX_COLLECTIONS) {
      fprintf(stderr,
              "No final mark at the bound was abandoned in %lu"
              " collections (%lu final marks, %lu abandoned)\n",
              marked, (unsigned long)n_finals, abandoned);
      CHECK(0);
    }
    /*
     * Until a final mark at the bound is abandoned (and after it with
     * `BOUND_STEADY`): a new list once a collection marks in steps and
     * after each abandoned final mark, and before the next collection.
     */
    if (bound_abandoned > 0 && BOUND_WAIT == mode) {
      bound_abandoned_at_wait = bound_abandoned;
      wait_for_collection();
      continue;
    }
    if (bound_abandoned > 0 && BOUND_ALLOC == mode)
      continue;
    if (bound_abandoned > 0)
      after_bound = TRUE;
    if (in_progress && (gc_no != list_gc_no || abandoned > abandoned_at)) {
      new_list();
      list_gc_no = gc_no;
      abandoned_at = abandoned;
    } else if (!in_progress && marked > marked_at) {
      new_list();
      marked_at = marked;
    }
  }
  GC_set_on_collection_event(0);
  list = NULL;
  printf("Last part%s: %lu collections, %lu final marks, %lu abandoned,"
         " %lu at the bound (%lu abandoned), %lu without a time limit after"
         " those, heap %lu MiB\n",
         BOUND_ALLOC == mode    ? ""
         : BOUND_STEADY == mode ? " (new lists)"
                                : " (waiting)",
         n_marked, n_finals, n_finals_abandoned, n_bound, n_bound_abandoned,
         n_capped, (unsigned long)(GC_get_heap_size() / MiB));
  /* The collection completed after the bound, not in one stop. */
  CHECK(1 == completed_after_bound);
  if (BOUND_STEADY == mode) {
    /*
     * Exactly one final mark without a time limit, after exactly
     * `MAX_ABANDONED_AT_BOUND` abandoned at the bound.
     */
    CHECK(1 == n_capped);
    CHECK(MAX_ABANDONED_AT_BOUND == n_bound_abandoned);
  }
  if (BOUND_WAIT == mode)
    CHECK(n_bound_abandoned <= bound_abandoned_at_wait + 1);
}
#endif

int
main(void)
{
  GC_stop_func old_stop_func;
  GC_word ceiling_percent;
  unsigned long i;
  unsigned first, last;
  int steps, sized, full_freq, max_attempts;

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
  ceiling_percent = GC_get_heap_ceiling_percent();
  full_freq = GC_get_full_freq();
  max_attempts = GC_get_max_prior_attempts();
  GC_set_heap_ceiling_percent(0);

  GC_set_time_limit(1);
  GC_set_max_prior_attempts(1);
  GC_set_stop_func(stop_marks);
  first = run_part(1, 0, 0);
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
  (void)run_part(2, steps ? 2 : 1, steps ? 2 : 1);

  GC_set_time_limit(GC_TIME_UNLIMITED);
  GC_set_max_prior_attempts(1);
  GC_set_full_freq(0);
  GC_set_stop_func(stop_marks);
  (void)run_part(3, steps ? 3 : 1, steps ? 3 : 1);

  GC_set_heap_ceiling_percent(ceiling_percent);
  GC_set_time_limit(1);
  GC_set_stop_func(stop_marks);
  sized = GC_get_memory_limit() != 0 && ceiling_percent != 0;
  last = run_part(4, steps ? 2 : 1, !steps ? 1 : sized ? 4 : 5);
  if (n_stops_at_bound != 0) {
    fprintf(stderr,
            "Part 4: the stop function was called %lu times during"
            " final marks at the bound\n",
            n_stops_at_bound);
    exit(1);
  }

  GC_set_stop_func(old_stop_func);
  GC_set_on_collection_event(0);
  printf("Collections in %u, %u, %u and %u (the first of the part)"
         " world-stopped marks, %u more for final marks abandoned in the"
         " second part, heap %lu MiB\n",
         first, steps ? 2U : 1U, steps ? 3U : 1U, last, n_abandoned,
         (unsigned long)(GC_get_heap_size() / MiB));

  GC_set_full_freq(full_freq);
  GC_set_max_prior_attempts(max_attempts);
#if defined(PARALLEL_MARK_DEADLINE) && !defined(_WIN32)
  if (0 == GC_get_memory_limit()) {
    printf("The memory limit is unknown, the last part skipped\n");
  } else {
    run_bound_part(BOUND_ALLOC);
    run_bound_part(BOUND_STEADY);
    run_bound_part(BOUND_WAIT);
  }
#else
  printf("The parallel marker or the clock is unavailable, or the clock"
         " counts milliseconds, the last part skipped\n");
#endif
  printf("SUCCEEDED\n");
  return 0;
}
