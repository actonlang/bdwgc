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
 * Test the set-up (the sweep of the blocks left unswept, then the
 * clearing of the mark bits) and the finish (the emptying of the free
 * lists, then the walk of the blocks) of collections in chunks, with the
 * client running between them: the incremental mode with the manual VDB,
 * every collection full and a time limit of 10 microseconds, then a time
 * limit of 2 ms, first with every collection full (the set-up of which
 * sweeps the blocks left unswept into the free lists, so that the free
 * lists are long when the finish empties them), then with minor
 * collections (whose finish finds blocks left unswept on the reclaim
 * lists).  Old objects in many blocks keep young objects.  Between the
 * chunks (and the steps of the marking) the test allocates garbage of
 * several sizes and objects filled with their own number, which it keeps
 * in a ring (also large ones), replaces young objects (with a barrier),
 * frees an old object and its young object explicitly and replaces both,
 * and looks for pending work: `GC_collect_a_little()` reports collection
 * work left, and no world-stopped mark has started since the start of the
 * latest full collection (a pending set-up), or a finish has started and
 * not ended (a pending finish).  It starts a collection explicitly at one
 * of the first looks at every other set-up seen pending, which replaces
 * the set-up, and at every 4th pending finish seen, which completes the
 * finish.  At every 4th pending finish seen from the 2nd on, it disables
 * the collections and calls `GC_collect_a_little()` without allocating
 * until it reports no work left: the finish must have ended then, as its
 * chunks run while the collections are disabled too.  At every 4th
 * pending finish seen from the 3rd on, it grows the heap, which makes
 * a collection due, and calls `GC_collect_a_little()`: no world-stopped
 * mark may start while a finish is pending.  At one look at each of the
 * other set-ups it calls `GC_finalize_all()`, which marks finalizable
 * objects (large objects that a static root keeps) and the objects they
 * reference, then stores new young objects into them (with a barrier):
 * a set-up that has cleared mark bits must clear them again, else the
 * marking finds these objects marked and does not scan them.  Every
 * explicit collection must complete its finish.  Every old and
 * finalizable object must keep its young object, also at the end of every
 * marking (by the mark bits), and every object in the ring its contents:
 * a sweep that read partly cleared mark bits would free live objects,
 * which are then reused, and an object handed out twice, or freed while
 * in use, is overwritten.  With the parallel marker (only with it a
 * collection is marked in steps, set up and finished in chunks), except
 * on Windows, where the clock of the collector counts milliseconds,
 * explicit collections must have replaced pending set-ups, set-ups seen
 * pending must have completed on their own, a finish must have been
 * pending (it ended in a later round of the test than the one it started
 * in), one must have run with the collections disabled, and one with
 * a collection due, unless the write-barrier checker runs.  There, in
 * every other set-up that calls `GC_finalize_all()`, the test then waits
 * for the set-up without allocating, so that its chunks are due only by
 * the time limit: the set-up must complete within `MAX_WAIT_CALLS` calls
 * of `GC_collect_a_little()`.  Under the write-barrier checker, the finish
 * of a collection runs at once: no finish may have been pending.  Without
 * the manual VDB mode or the clock, or if `NO_INCREMENTAL` is defined, the
 * test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc/gc_mark.h"
#include "gc/javaxfc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NO_CLOCK
#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define MAGIC 0x5e7a9L

/* The old objects, about 30 of them per 16 KiB block (1000 blocks). */
#  define N_OLD 30000

/* The finalizable objects, among the old ones in the heap. */
#  define N_FIN 8
#  define FIN_KEY(j) ((GC_word)N_OLD + (GC_word)(j))

/* The collections to run in each part. */
#  define N_COLLECTIONS 40

/* The explicit collections, and the checks of every object. */
#  define COLLECT_EVERY 20000
#  define CHECK_EVERY 5000

/* The objects kept in the ring. */
#  define N_RING 4096

/*
 * The rounds between two looks for pending work, and the pending finishes
 * seen per explicit collection then.
 */
#  define LOOK_EVERY 8
#  define COLLECT_PENDING_EVERY 4

/*
 * The calls of `GC_collect_a_little()` that a set-up, or a finish with the
 * collections disabled, takes at most.  The clock of the collector counts
 * milliseconds on Windows (`GetTickCount64()` or `GetTickCount()`): a chunk
 * that only the time limit makes due runs once the clock advances, up to
 * 15.6 ms or several hundred thousand calls later.
 */
#  ifdef _WIN32
#    define MAX_WAIT_CALLS 100000000UL
#  else
#    define MAX_WAIT_CALLS 1000000UL
#  endif

/*
 * Whether a set-up or a finish spans several chunks.  Only with the
 * parallel marker a collection is marked in steps, set up and finished in
 * chunks.  The clock of the collector counts milliseconds on Windows
 * (`GetTickCount64()` or `GetTickCount()`), thus a time limit lasts until
 * the next tick there, and a set-up or a finish seldom spans two chunks.
 */
#  if defined(PARALLEL_MARK) && !defined(_WIN32)
#    define IN_CHUNKS 1
#  else
#    define IN_CHUNKS 0
#  endif

#  if !defined(GC_NO_FINALIZATION) && !defined(JAVA_FINALIZATION_NOT_NEEDED)
#    define FINALIZE_ALL 1
#  else
#    define FINALIZE_ALL 0
#  endif

struct young {
  GC_word magic;
  GC_word key;
};

struct old {
  struct young *p;
  GC_word key;
  void *pad[60];
};

/* A large object at any block size. */
struct fin {
  struct young *p;
  GC_word key;
  char pad[40000];
};

static struct old *volatile olds[N_OLD];
static struct fin *volatile fins[N_FIN];

/* The ring of kept objects, and the size of each, in words. */
static GC_word *volatile ring[N_RING];
static size_t ring_words[N_RING];
static GC_word ring_next;

/* The round of the test in progress. */
static volatile unsigned long cur_round;

/*
 * The world-stopped marks started, their count at the start of the latest
 * full collection, and the full collections started.  Whether the test
 * runs an explicit collection, whether the set-up of the latest full
 * collection was seen pending, the set-ups seen pending that started their
 * marking on their own, and the finalizable object (if any) that a marking
 * completed with, marked but not its young object.  The starts and the
 * ends of finishes, the round of the latest start, the finishes which
 * ended in a later round than they started in, and whether a world-stopped
 * mark started while a finish was pending.  Updated by the collector
 * holding its lock.
 */
static volatile unsigned long n_mark_starts;
static volatile unsigned long n_mark_starts_at_full;
static volatile unsigned long n_full_starts;
static volatile int explicit_collection;
static volatile int seen_pending;
static volatile unsigned long n_setups_done;
static volatile int young_unmarked = -1;
static volatile unsigned long n_finish_starts;
static volatile unsigned long n_finish_ends;
static volatile unsigned long finish_start_round;
static volatile unsigned long n_finish_pending;
static volatile int mark_in_finish;

/*
 * The set-ups seen pending, the full collections started at the latest
 * one, its looks, all looks at pending set-ups, the set-ups replaced by an
 * explicit collection, and the calls of `GC_finalize_all()`; the pending
 * finishes seen, the explicit collections then, the finishes run with the
 * collections disabled, and those with a collection made due.
 */
static unsigned long n_setups, full_starts, n_looks, n_setup_looks;
static unsigned long n_replaced, n_finalize_all;
static unsigned long n_finish_seen, n_finish_collected, n_finish_disabled;
static unsigned long n_finish_due;

static void GC_CALLBACK
on_event(GC_EventType e)
{
  int j;

  switch (e) {
  case GC_EVENT_MARK_START:
    if (n_mark_starts == n_mark_starts_at_full && seen_pending
        && !explicit_collection)
      n_setups_done++;
    if (n_finish_starts != n_finish_ends)
      mark_in_finish = 1;
    n_mark_starts++;
    break;
  case GC_EVENT_MARK_END:
    for (j = 0; j < N_FIN; j++) {
      struct fin *f = fins[j];

      if (f != NULL && GC_is_marked(GC_base(f))
          && !GC_is_marked(GC_base(f->p)))
        young_unmarked = j;
    }
    break;
  case GC_EVENT_RECLAIM_START:
    n_finish_starts++;
    finish_start_round = cur_round;
    break;
  case GC_EVENT_RECLAIM_END:
    n_finish_ends++;
    if (cur_round != finish_start_round)
      n_finish_pending++;
    break;
  default:
    break;
  }
}

static void GC_CALLBACK
on_full_start(void)
{
  n_mark_starts_at_full = n_mark_starts;
  n_full_starts++;
  seen_pending = 0;
}

/*
 * Do some collection work, if due, and return whether the set-up of
 * a full collection is pending.
 */
static int
setup_pending(void)
{
  return GC_collect_a_little() && n_mark_starts == n_mark_starts_at_full;
}

/*
 * Wait for the pending set-up without allocating: its chunks are then due
 * only by the time limit since the end of the previous chunk.
 */
static void
wait_for_setup(void)
{
  unsigned long n;

  for (n = 0; setup_pending(); n++)
    CHECK(n < MAX_WAIT_CALLS);
}

/*
 * Run the pending finish with the collections disabled, without
 * allocating: its chunks are then due only by the time limit since the
 * end of the previous chunk.
 */
static void
finish_disabled(void)
{
  unsigned long n;

  GC_disable();
  for (n = 0; GC_collect_a_little(); n++)
    CHECK(n < MAX_WAIT_CALLS);
  CHECK(n_finish_starts == n_finish_ends);
  GC_enable();
  n_finish_disabled++;
}

/*
 * Make a collection due while the finish is pending, then do some
 * collection work: the collection must not start until the finish has
 * ended.  A growth of the heap since the latest collection makes
 * a collection due, a reason that `GC_should_collect()` does not consume.
 */
static void
make_collection_due(void)
{
  CHECK(GC_expand_hp(65536));
  (void)GC_collect_a_little();
  n_finish_due++;
}

/* Collect explicitly, which completes the finish too. */
static void
collect(void)
{
  explicit_collection = 1;
  GC_gcollect();
  explicit_collection = 0;
  CHECK(n_finish_starts == n_finish_ends);
}

static void
set_time_limit_us(unsigned long us)
{
  struct GC_timeval_s tv;

  tv.tv_ms = us / 1000;
  tv.tv_nsec = us % 1000 * 1000;
  GC_set_time_limit_tv(tv);
}

static struct young *
new_young(GC_word key)
{
  struct young *y = (struct young *)GC_malloc(sizeof(struct young));

  CHECK(y != NULL);
  y->magic = MAGIC;
  y->key = key;
  return y;
}

static struct old *
new_old(GC_word key)
{
  struct old *o = (struct old *)GC_malloc(sizeof(struct old));

  CHECK(o != NULL);
  o->key = key;
  /* No barrier: a new object. */
  o->p = new_young(key);
  return o;
}

static struct fin *
new_fin(GC_word key)
{
  struct fin *f = (struct fin *)GC_MALLOC(sizeof(struct fin));

  CHECK(f != NULL);
  f->key = key;
  f->p = new_young(key);
  return f;
}

/* Allocate an object of `words` words and fill it with its number. */
static GC_word *
new_filled(size_t words, GC_word key)
{
  GC_word *p = (GC_word *)GC_MALLOC_ATOMIC(words * sizeof(GC_word));
  size_t j;

  CHECK(p != NULL);
  for (j = 0; j < words; j++)
    p[j] = key;
  return p;
}

#  if FINALIZE_ALL
static void GC_CALLBACK
on_finalize(void *obj, void *client_data)
{
  (void)obj;
  (void)client_data;
}

/*
 * Mark the finalizable objects by `GC_finalize_all()`, then store a new
 * young object into each of them.
 */
static void
finalize_all(void)
{
  int j;

  for (j = 0; j < N_FIN; j++)
    GC_REGISTER_FINALIZER(fins[j], on_finalize, NULL, NULL, NULL);
  GC_finalize_all();
  for (j = 0; j < N_FIN; j++) {
    struct fin *f = fins[j];
    struct young *y = new_young(FIN_KEY(j));

    f->p = y;
    GC_END_STUBBORN_CHANGE(&f->p);
    GC_reachable_here(y);
  }
}
#  else
#    define finalize_all() (void)0
#  endif

/* Overwrite the stack below the caller, to drop stale pointers. */
static void
clear_stack(void)
{
  volatile char buf[16384];

  memset((char *)buf, 0, sizeof(buf));
  GC_noop1((GC_word)(GC_uintptr_t)buf);
}

/*
 * Look at the pending set-up: an odd set-up is replaced at its look 2 to
 * 4, an even one calls `GC_finalize_all()` at its look 1 to 4, and every
 * other even one then waits until it completes.
 */
static void
look_at_setup(void)
{
  if (n_full_starts != full_starts) {
    full_starts = n_full_starts;
    n_setups++;
    n_looks = 0;
  }
  n_looks++;
  n_setup_looks++;
  seen_pending = 1;
  if (n_setups % 2 != 0) {
    if (n_looks == 2 + n_setups / 2 % 3) {
      collect();
      n_replaced++;
    }
  } else if (n_looks == 1 + n_setups / 2 % 4) {
    if (FINALIZE_ALL) {
      finalize_all();
      clear_stack();
      n_finalize_all++;
    }
    if (IN_CHUNKS && n_setups % 4 == 0)
      wait_for_setup();
  }
}

static void
check_young(const char *what, GC_word i, const struct young *y)
{
  if (NULL == y || y->magic != MAGIC || y->key != i) {
    fprintf(stderr, "%s %lu lost its young object\n", what, (unsigned long)i);
    exit(1);
  }
}

static void
check_all(void)
{
  GC_word i;

  for (i = 0; i < N_OLD; i++) {
    CHECK(olds[i]->key == i);
    check_young("Old object", i, olds[i]->p);
  }
  for (i = 0; i < N_FIN; i++) {
    CHECK(fins[i]->key == FIN_KEY(i));
    check_young("Finalizable object", FIN_KEY(i), fins[i]->p);
  }
  for (i = 0; i < N_RING; i++) {
    const GC_word *p = ring[i];
    size_t j;

    if (NULL == p)
      continue;
    for (j = 0; j < ring_words[i]; j++) {
      if (p[j] != p[0] || p[0] % N_RING != i) {
        fprintf(stderr, "Object %lu of the ring overwritten\n",
                (unsigned long)i);
        exit(1);
      }
    }
  }
  if (mark_in_finish) {
    fprintf(stderr, "A marking started while a finish was pending\n");
    exit(1);
  }
  if (young_unmarked >= 0) {
    fprintf(stderr,
            "A marking completed with finalizable object %d marked but not"
            " its young object\n",
            young_unmarked);
    exit(1);
  }
}

/*
 * Run collections until `n_collections` have completed, and return the
 * finishes which were pending meanwhile.  An explicit collection runs
 * every `COLLECT_EVERY` rounds, if `after_progress` only when a collection
 * has completed since the previous one.
 */
static unsigned long
run(unsigned long n_collections, int after_progress)
{
  GC_word start = GC_get_gc_no();
  unsigned seed = 1;
  unsigned long round;
  unsigned long pending_at_start = n_finish_pending;
  GC_word explicit_gc_no = GC_get_gc_no();

  for (round = 1; GC_get_gc_no() - start < n_collections; round++) {
    GC_word i;
    unsigned k;

    CHECK(round < 100000000UL);
    cur_round++;
    for (k = 0; k < 16; k++)
      CHECK(GC_MALLOC(16 + 16 * (k & 7)) != NULL);
    seed = seed * 1103515245U + 12345U;
    i = (GC_word)((seed >> 8) % N_OLD);
    if (((seed >> 24) & 3) == 0) {
      struct old *o = olds[i];
      struct young *y = new_young(i);

      o->p = y;
      GC_END_STUBBORN_CHANGE(&o->p);
      GC_reachable_here(y);
    } else if (((seed >> 24) & 15) == 1) {
      struct old *o = olds[i];

      olds[i] = new_old(i);
      /*
       * By `GC_free()`, as the old and the young objects are allocated
       * without a debug header: `GC_FREE()` is `GC_debug_free()` with
       * `GC_DEBUG`, which leaves a collectable object to the collector.
       */
      GC_free(o->p);
      GC_free(o);
    }
    {
      GC_word key = ring_next++;
      size_t r = (size_t)(key % N_RING);
      /* Mostly small objects, now and then a large one. */
      size_t words = ((seed >> 4) & 63) == 0 ? 3000 : 2 + ((seed >> 12) & 31);

      ring[r] = new_filled(words, key);
      ring_words[r] = words;
    }
    if (round % LOOK_EVERY == 0 && GC_collect_a_little()) {
      if (n_mark_starts == n_mark_starts_at_full) {
        look_at_setup();
      } else if (n_finish_starts != n_finish_ends) {
        /* A finish is pending. */
        if (++n_finish_seen % COLLECT_PENDING_EVERY == 0) {
          collect();
          n_finish_collected++;
        } else if (n_finish_seen % COLLECT_PENDING_EVERY == 2) {
          finish_disabled();
        } else if (n_finish_seen % COLLECT_PENDING_EVERY == 3) {
          make_collection_due();
        }
      }
    }
    if (round % COLLECT_EVERY == 0
        && (!after_progress || GC_get_gc_no() != explicit_gc_no)) {
      collect();
      explicit_gc_no = GC_get_gc_no();
    }
    if (round % CHECK_EVERY == 0)
      check_all();
  }
  check_all();
  return n_finish_pending - pending_at_start;
}

int
main(void)
{
  GC_word i;
  unsigned long n_pending_setup, n_pending_full, n_pending_minor;

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
  set_time_limit_us(10);
  GC_set_full_freq(0);
#  if defined(PARALLEL_MARK) && defined(_WIN32)
  /*
   * The clock of the collector counts milliseconds on Windows, in ticks of
   * about 15.6 ms (`GetTickCount64()`, or `GetTickCount()` where `GC_WINNT` is
   * not defined, as in 32-bit builds by MinGW GCC or clang).  A step of the
   * marking waits after the latest slice for a spacing that this clock reads
   * as passed only at its next tick, thus away from the heap cap a step starts
   * at most once per tick, and the marking falls behind the allocation.
   * Without an allocation budget, the heap cap is the live data plus half of
   * the distance from it to the heap ceiling, about 1.5 GiB in a 32-bit
   * process with 4 GiB of memory or more, and the heap of this test grows far
   * beyond its live data.  Every request of memory from the system takes one
   * of the `MAX_HEAP_SECTS` slots there, 512 in a 32-bit build without
   * `LARGE_CONFIG` and `SMALL_CONFIG`, and the collector aborts once they run
   * out.  A budget of 100% puts the cap at twice the live data, and near the
   * cap the marking and the finish complete in one stop, thus the finish is
   * seldom pending here.  Elsewhere the test runs without a budget, as it
   * checks there that the finish was pending (see `IN_CHUNKS`).
   */
  GC_set_alloc_budget_percent(100);
#  endif
  for (i = 0; i < N_OLD; i++) {
    if (i % (N_OLD / N_FIN) == 0)
      fins[i / (N_OLD / N_FIN)] = new_fin(FIN_KEY(i / (N_OLD / N_FIN)));
    olds[i] = new_old(i);
  }
  /*
   * Complete the collection work left before setting the callbacks:
   * a collection in progress could pass for a set-up, and `on_event()`
   * would count the end of a pending finish without its start.
   */
  GC_gcollect();
  GC_set_on_collection_event(on_event);
  GC_set_start_callback(on_full_start);
  n_pending_setup = run(N_COLLECTIONS, 0);
  /*
   * With steps of 2 ms, spaced by the pacing, a collection marks over more
   * than `COLLECT_EVERY` rounds: the explicit collections, each of which
   * completes the marking of the collection in progress and its finish at
   * once, would catch every collection while it marks.
   */
  set_time_limit_us(2000);
  n_pending_full = run(N_COLLECTIONS, 1);
  GC_set_full_freq(1000);
  n_pending_minor = run(N_COLLECTIONS, 1);
  GC_set_start_callback(0);
  GC_set_on_collection_event(0);
  printf("%lu pending set-ups seen, %lu replaced by an explicit collection,"
         " %lu completed on their own, %lu with GC_finalize_all(); finish"
         " pending %lu, %lu and %lu times in the three parts, %lu explicit"
         " collections then, %lu finishes with the collections disabled,"
         " %lu with a collection due\n",
         n_setup_looks, n_replaced, (unsigned long)n_setups_done,
         n_finalize_all, n_pending_setup, n_pending_full, n_pending_minor,
         n_finish_collected, n_finish_disabled, n_finish_due);
#  if IN_CHUNKS
  if (0 == n_replaced || 0 == n_setups_done) {
    fprintf(stderr, "Set-ups in chunks were not replaced or completed\n");
    exit(1);
  }
  if ((0 == n_pending_setup + n_pending_full + n_pending_minor
       || 0 == n_finish_disabled || 0 == n_finish_due)
      && NULL == getenv("GC_WBAR_VERIFY")) {
    fprintf(stderr, "The finish was never pending, or never with the"
                    " collections disabled or a collection due\n");
    exit(1);
  }
#  endif
  if (n_pending_setup + n_pending_full + n_pending_minor != 0
      && getenv("GC_WBAR_VERIFY") != NULL) {
    fprintf(stderr, "The finish was pending while the write-barrier checker"
                    " runs\n");
    exit(1);
  }
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The clock is unavailable, test skipped\n");
  return 0;
}
#endif
