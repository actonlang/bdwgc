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
 * Test full collections whose set-up (the sweep of the blocks left
 * unswept and the clearing of the mark bits) runs in chunks, with the
 * client running between them: the incremental mode with the manual VDB,
 * a time limit of 10 microseconds, and every collection full.  Old
 * objects in many blocks keep young objects.  Between the chunks (and
 * the steps of the marking) the test allocates garbage of several sizes,
 * replaces young objects (with a barrier), frees an old object and its
 * young object explicitly and replaces both, and looks for a pending
 * set-up: `GC_collect_a_little()` reports collection work left, and no
 * world-stopped mark has started since the start of the latest full
 * collection.  It starts a collection explicitly at one of the first
 * looks at every other set-up seen pending, which replaces the set-up.
 * At one look at each of the others it calls `GC_finalize_all()`, which
 * marks finalizable objects (large objects that a static root keeps) and
 * the objects they reference, then stores new young objects into them
 * (with a barrier):
 * a set-up that has cleared mark bits must clear them again, else the
 * marking finds these objects marked and does not scan them.  Every old
 * and finalizable object must keep its young object, also at the end of
 * every marking (by the mark bits): a sweep that read partly cleared mark
 * bits would free live objects, which are then reused.  With the parallel
 * marker (only with it a collection is marked in steps and set up in
 * chunks), except on Windows, where the clock of the collector counts
 * milliseconds, explicit collections must have replaced pending set-ups,
 * and set-ups seen pending must have completed on their own.  There, in
 * every other set-up that calls `GC_finalize_all()`, the test then waits
 * for the set-up without allocating, so that its chunks are due only by
 * the time limit: the set-up must complete within `MAX_SETUP_CALLS` calls
 * of `GC_collect_a_little()`.  Without the manual VDB mode or the clock,
 * or if `NO_INCREMENTAL` is defined, the test is skipped.
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

/* The collections to run. */
#  define N_COLLECTIONS 40

/* The explicit collections, and the checks of every old object. */
#  define COLLECT_EVERY 20000
#  define CHECK_EVERY 5000

/* The rounds between two looks for a pending set-up. */
#  define LOOK_EVERY 8

/* The calls of `GC_collect_a_little()` that a set-up takes at most. */
#  define MAX_SETUP_CALLS 1000000UL

/*
 * Whether a set-up spans several chunks.  Only with the parallel marker
 * a collection is marked in steps and set up in chunks.  The clock of the
 * collector counts milliseconds on Windows (`GetTickCount64()`), thus
 * a time limit of 10 microseconds lasts until the next tick there, and
 * a set-up seldom spans two chunks.
 */
#  if defined(PARALLEL_MARK) && !defined(_WIN32)
#    define SETUP_IN_CHUNKS 1
#  else
#    define SETUP_IN_CHUNKS 0
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

/*
 * The world-stopped marks started, their count at the start of the latest
 * full collection, and the full collections started.  Whether the test
 * runs an explicit collection, whether the set-up of the latest full
 * collection was seen pending, the set-ups seen pending that started their
 * marking on their own, and the finalizable object (if any) that a marking
 * completed with, marked but not its young object.  Updated by the
 * collector holding its lock.
 */
static volatile unsigned long n_mark_starts;
static volatile unsigned long n_mark_starts_at_full;
static volatile unsigned long n_full_starts;
static volatile int explicit_collection;
static volatile int seen_pending;
static volatile unsigned long n_setups_done;
static volatile int young_unmarked = -1;

static void GC_CALLBACK
on_event(GC_EventType e)
{
  int j;

  switch (e) {
  case GC_EVENT_MARK_START:
    if (n_mark_starts == n_mark_starts_at_full && seen_pending
        && !explicit_collection)
      n_setups_done++;
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
    CHECK(n < MAX_SETUP_CALLS);
}

static void
collect(void)
{
  explicit_collection = 1;
  GC_gcollect();
  explicit_collection = 0;
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
  if (young_unmarked >= 0) {
    fprintf(stderr,
            "A marking completed with finalizable object %d marked but not"
            " its young object\n",
            young_unmarked);
    exit(1);
  }
}

int
main(void)
{
  struct GC_timeval_s tv;
  GC_word i, start;
  unsigned seed = 1;
  unsigned long round, n_pending = 0, n_explicit = 0, n_finalize_all = 0;
  unsigned long full_starts = 0, n_setups = 0, n_looks = 0;

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
  tv.tv_ms = 0;
  tv.tv_nsec = 10 * 1000;
  GC_set_time_limit_tv(tv);
  GC_set_full_freq(0);
  for (i = 0; i < N_OLD; i++) {
    if (i % (N_OLD / N_FIN) == 0)
      fins[i / (N_OLD / N_FIN)] = new_fin(FIN_KEY(i / (N_OLD / N_FIN)));
    olds[i] = new_old(i);
  }
  GC_set_on_collection_event(on_event);
  GC_set_start_callback(on_full_start);
  start = GC_get_gc_no();
  for (round = 1; GC_get_gc_no() - start < N_COLLECTIONS; round++) {
    unsigned k;

    CHECK(round < 100000000UL);
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
    if (round % LOOK_EVERY == 0 && setup_pending()) {
      if (n_full_starts != full_starts) {
        full_starts = n_full_starts;
        n_setups++;
        n_looks = 0;
      }
      n_looks++;
      n_pending++;
      seen_pending = 1;
      /*
       * The look at which an odd set-up is replaced (the 2nd to the 4th),
       * or an even one calls `GC_finalize_all()` (the 1st to the 4th), and
       * every other even one then waits until it completes.
       */
      if (n_setups % 2 != 0) {
        if (n_looks == 2 + n_setups / 2 % 3) {
          collect();
          n_explicit++;
        }
      } else if (n_looks == 1 + n_setups / 2 % 4) {
        if (FINALIZE_ALL) {
          finalize_all();
          clear_stack();
          n_finalize_all++;
        }
        if (SETUP_IN_CHUNKS && n_setups % 4 == 0)
          wait_for_setup();
      }
    }
    if (round % COLLECT_EVERY == 0)
      collect();
    if (round % CHECK_EVERY == 0)
      check_all();
  }
  check_all();
  GC_set_start_callback(0);
  GC_set_on_collection_event(0);
  printf("%lu collections, %lu rounds, %lu pending set-ups seen, %lu"
         " replaced by an explicit collection, %lu completed on their own,"
         " %lu with GC_finalize_all()\n",
         (unsigned long)(GC_get_gc_no() - start), round, n_pending, n_explicit,
         (unsigned long)n_setups_done, n_finalize_all);
#  if SETUP_IN_CHUNKS
  if (0 == n_explicit || 0 == n_setups_done) {
    fprintf(stderr, "Set-ups in chunks were not replaced or completed\n");
    exit(1);
  }
#  endif
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
