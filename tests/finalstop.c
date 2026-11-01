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
 * start, also with `GC_get_max_prior_attempts()` at 0: each collection takes
 * 2 marks.
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

/* The time limit of the second part, in milliseconds. */
#define LIMIT_MS 100

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

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  if (0 == part)
    return;
  switch (e) {
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
  case GC_EVENT_POST_START_WORLD:
    /* A final mark that follows the step starts long after the step. */
    if (2 == part && !marking)
      sleep_ms(2 * LIMIT_MS);
    marking = 0;
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
 * is 0.  Return the marks of the first collection.
 */
static unsigned
run_part(int p, unsigned marks)
{
  GC_word start;
  unsigned long i;
  unsigned k, big_for = (unsigned)-1;

  n_marks = 0;
  n_counted = 0;
  part = p;
  start = GC_get_gc_no();
  for (i = 0; GC_get_gc_no() - start < N_COLLECTIONS; i++) {
    junk = GC_MALLOC(sizeof(struct node));
    CHECK(junk != NULL);
    if (2 == p && 1 == n_marks && big_for != n_counted) {
      /* The first mark was abandoned: only a root holds the new object. */
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
  if (0 == marks)
    marks = marks_of[0];
  for (k = 0; k < N_COLLECTIONS; k++) {
    if (marks_of[k] != marks) {
      fprintf(stderr,
              "Part %d: collection %u took %u world-stopped marks, not %u\n",
              p, k + 1, marks_of[k], marks);
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
  printf("Collections in %u, %u and %u world-stopped marks, heap %lu MiB\n",
         first, steps ? 2U : 1U, steps ? 3U : 1U,
         (unsigned long)(GC_get_heap_size() / MiB));
  printf("SUCCEEDED\n");
  return 0;
}
