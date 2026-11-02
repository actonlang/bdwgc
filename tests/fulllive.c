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
 * Test the full collections that `GC_GEN_FULL_LIVE_PCT` makes due in the
 * incremental mode: the manual VDB mode without a time limit, and no full
 * collection by the count of minor ones.  The client keeps a live tree and
 * a ring of its latest objects, and replaces the objects of the ring
 * before each collection.  A minor collection marks the objects in the
 * ring, which die at the next refill but stay marked until a full
 * collection, thus the bytes in use after each minor collection grow.
 * With the percentage 0, no collection is full when nothing is allocated
 * after a minor collection whose bytes in use the test sets as those after
 * the latest full collection, as the bytes in use stay the same, nor when
 * the bytes in use after the latest full collection are 0, as after the one
 * in `GC_init()`, which finds nothing in use.  Then the next collection
 * must be full if and only if the growth of the heap made it due or, after
 * a minor collection, the bytes in use exceed those after the latest full
 * collection by more than the percentage: the one of the variable (50 if
 * it is unset, run only if this test can check it), 20, and -1, which
 * makes no collection full, with a free space divisor of 1, so that the
 * growth of the heap makes no collection due, then a percentage never
 * exceeded with a divisor of 3, so that the growth of the heap alone makes
 * collections full.  This test includes the collector source to set the
 * percentage and the bytes in use after the latest full collection, and to
 * read the bytes in use and the state of the heap-growth trigger.  Skipped
 * without the manual VDB mode.
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
/*
 * The collector is compiled as a part of this program, thus gcc may
 * inline `GC_noop1_ptr()` into `os_main_stackbottom()` (if `CPPCHECK` is
 * defined) and report storing the address of a local variable to a global
 * one; the stored pointer is never used.
 */
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

#if !defined(NO_MANUAL_VDB) && !defined(NO_INCREMENTAL)

/* The nodes of the live tree, of 4 words each. */
#  define N_NODES (1 << 17)

/* The objects in the ring. */
#  define N_RING (1 << 13)

/* The collections recorded at most. */
#  define MAX_COLLECTIONS 400

/*
 * The full collections to run after the first one of a run (only those
 * the growth of the heap alone makes due, if it may), or the minor
 * collections over half the bytes in use after it, if the percentage is
 * negative.  Also the collections of each part of `run_pct_0()`, and the
 * minor collections in a row that keep the bytes in use before them.
 */
#  define N_WANTED 3

/*
 * The minor collections that `run_pct_0()` runs at most until `N_WANTED`
 * in a row keep the bytes in use.
 */
#  define MAX_SETTLE (4 * N_WANTED)

/*
 * The range of the percentages that the run with the one of the variable
 * can check.  Below `MIN_PCT`, the first minor collection after a full one
 * can make the next one full, which `check_run()` does not allow: it adds
 * a refill of the ring, under 1/17 of the bytes in use after the full
 * collection, and the objects left on the thread-local free lists, up to
 * about as many again.  Above `MAX_PCT`, the growth of the heap makes
 * a collection due before the percentage does.
 */
#  define MIN_PCT 15
#  define MAX_PCT 200

struct node {
  struct node *left;
  struct node *right;
  GC_word value;
  GC_word pad;
};

static struct node *volatile root;
static struct node **volatile ring;

/*
 * A collection: whether it was full, the bytes in use after it, whether
 * the growth of the heap made the next collection due, whether the
 * collector made the next collection full (`GC_need_full_gc`), and the
 * percentage after it.
 */
struct collection {
  GC_bool full;
  GC_bool heap_growth;
  GC_bool need_full;
  word in_use;
  int pct;
};

/* The collections recorded.  Protected by the allocator lock. */
static struct collection colls[MAX_COLLECTIONS];
static unsigned n_colls;

/* Called with the allocator lock held. */
static void GC_CALLBACK
on_event(GC_EventType e)
{
  struct collection *c;

  if (n_colls >= MAX_COLLECTIONS)
    return;
  c = &colls[n_colls];
  if (GC_EVENT_MARK_END == e) {
    c->full = GC_is_full_gc;
  } else if (GC_EVENT_RECLAIM_END == e) {
    c->in_use = GC_composite_in_use + GC_atomic_in_use;
    c->heap_growth = GC_heapsize - GC_used_heap_size_after_full
                     > SIZET_SAT_ADD(min_bytes_allocd(), GC_large_free_bytes);
    c->need_full = GC_need_full_gc;
    c->pct = GC_gen_full_live_pct;
    n_colls++;
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
 * Replace the objects of the ring by new ones, every other one of them
 * pointer-free, so that the dead ones count in both `GC_composite_in_use`
 * and `GC_atomic_in_use`.
 */
static void
refill(GC_word value)
{
  unsigned i;

  for (i = 0; i < N_RING; i += 2) {
    GC_word *w = (GC_word *)GC_MALLOC_ATOMIC(sizeof(struct node));

    CHECK(w != NULL);
    *w = value;
    GC_PTR_STORE_AND_DIRTY(&ring[i], new_node(value));
    GC_PTR_STORE_AND_DIRTY(&ring[i + 1], w);
  }
}

static unsigned
locked_n_colls(void)
{
  unsigned n;

  LOCK();
  n = n_colls;
  UNLOCK();
  return n;
}

/* Return `after_full` plus `pct` percent of it, rounded down. */
static word
bound(word after_full, int pct)
{
  return after_full + after_full / 100 * (word)pct
         + after_full % 100 * (word)pct / 100;
}

/*
 * Check the collections recorded from the full collection `first` to
 * `n`: the collector makes the next collection full, and it is full, if
 * and only if the growth of the heap made it due or, after a minor
 * collection, the bytes in use exceed those after the latest full
 * collection by more than the percentage.  Unless `growth`, the growth of
 * the heap makes no collection due.  Two full collections are at least 3
 * collections apart.  Return the number of full collections after the
 * first one (only those the growth of the heap alone made due, if
 * `growth`), plus the number of minor collections whose bytes in use
 * exceed those after the latest full one by more than half while the
 * percentage is negative.
 */
static unsigned
check_run(unsigned first, unsigned n, GC_bool growth)
{
  word after_full = colls[first].in_use;
  unsigned i;
  unsigned n_minor = 0;
  unsigned result = 0;

  CHECK(colls[first].full);
  for (i = first + 1; i < n; i++) {
    const struct collection *prev = &colls[i - 1];
    GC_bool live = !prev->full && prev->pct >= 0
                   && prev->in_use > bound(after_full, prev->pct);
    GC_bool due = prev->heap_growth || live;

    CHECK(growth || !prev->heap_growth);
    CHECK(prev->need_full == due);
    CHECK(colls[i].full == due);
    if (due) {
      CHECK(n_minor >= 2);
      after_full = colls[i].in_use;
      n_minor = 0;
      if (!growth || !live)
        result++;
    } else {
      n_minor++;
      if (colls[i].pct < 0 && colls[i].in_use > after_full + after_full / 2)
        result++;
    }
  }
  return result;
}

static GC_word value;

/*
 * Set the percentage, collect fully, then refill the ring and collect
 * until `check_run()` returns `N_WANTED`.
 */
static void
run(int pct, GC_bool growth)
{
  unsigned first, n;

  LOCK();
  GC_gen_full_live_pct = pct;
  UNLOCK();
  GC_gcollect();
  first = locked_n_colls() - 1;
  do {
    refill(++value);
    GC_start_incremental_collection();
    n = locked_n_colls();
    CHECK(n < MAX_COLLECTIONS);
  } while (check_run(first, n, growth) < N_WANTED);
  printf("GC_GEN_FULL_LIVE_PCT=%d: %u collections, bytes in use after the"
         " first one %lu KiB\n",
         pct, n - first, (unsigned long)(colls[first].in_use >> 10));
}

/*
 * Check that none of the collections recorded from `first` to `n` was
 * full or made the next one full.
 */
static void
check_minor(unsigned first, unsigned n)
{
  unsigned i;

  for (i = first; i < n; i++)
    CHECK(!colls[i].full && !colls[i].need_full);
}

/*
 * Collect fully, then with the trigger off until `N_WANTED` minor
 * collections in a row keep the bytes in use, and make the bytes in use
 * after the latest of them the ones after the latest full collection.
 * A minor collection may mark a free object through a stale pointer: the
 * full collection emptied the global free lists, whose objects it marks
 * only to clear their marks again, and the collector leaves the addresses
 * of blocks, which are those of their first objects, in its stack frames.
 * These words change from one collection to the next until the
 * collections run the same way.  With the percentage 0, collect
 * `N_WANTED` times without allocation: the bytes in use stay the same.
 * Then set the bytes in use after the latest full collection to 0, refill
 * the ring and collect `N_WANTED` times.  None of the minor collections
 * may be full.  Called before any object dies, so that no collection
 * marks a dead object through a stale pointer, which would add to the
 * bytes in use.
 */
static void
run_pct_0(void)
{
  unsigned start, first, n, i, quiet;

  LOCK();
  GC_gen_full_live_pct = -1;
  UNLOCK();
  GC_gcollect();
  start = locked_n_colls();
  quiet = 0;
  do {
    GC_start_incremental_collection();
    n = locked_n_colls();
    CHECK(n <= start + MAX_SETTLE);
    quiet = colls[n - 1].in_use == colls[n - 2].in_use ? quiet + 1 : 0;
  } while (quiet < N_WANTED);
  check_minor(start, n);
  first = n - 1;
  LOCK();
  GC_in_use_after_full = colls[first].in_use;
  GC_gen_full_live_pct = 0;
  UNLOCK();
  for (i = 0; i < N_WANTED; i++)
    GC_start_incremental_collection();
  n = locked_n_colls();
  CHECK(n == first + 1 + N_WANTED);
  for (i = first + 1; i < n; i++)
    CHECK(colls[i].in_use == colls[first].in_use);
  check_minor(first, n);

  first = n;
  LOCK();
  GC_in_use_after_full = 0;
  UNLOCK();
  for (i = 0; i < N_WANTED; i++) {
    refill(++value);
    GC_start_incremental_collection();
  }
  n = locked_n_colls();
  CHECK(n == first + N_WANTED);
  check_minor(first, n);
}

int
main(void)
{
  const char *str;
  int pct;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  GC_enable_incremental();
  if (!GC_is_incremental_mode()
      || (GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  GC_set_time_limit(GC_TIME_UNLIMITED);
  GC_set_full_freq(100000);
  GC_set_free_space_divisor(1);
  GC_set_alloc_budget_percent(0);
  /* No heap growth, and no collection that follows one, while recording. */
  CHECK(GC_expand_hp(64 << 20));
  root = build(0, N_NODES);
  ring = (struct node **)GC_MALLOC(N_RING * sizeof(struct node *));
  CHECK(ring != NULL);
  refill(value);
  GC_set_on_collection_event(on_event);
  str = GETENV("GC_GEN_FULL_LIVE_PCT");
  pct = GC_gen_full_live_pct;
  CHECK(pct == (NULL == str ? 50 : atoi(str)));
  run_pct_0();
  if (pct >= MIN_PCT && pct <= MAX_PCT) {
    run(pct, FALSE);
  } else {
    printf("GC_GEN_FULL_LIVE_PCT=%d: not run, outside %d..%d\n", pct, MIN_PCT,
           MAX_PCT);
  }
  run(20, FALSE);
  run(-1, FALSE);
  GC_set_free_space_divisor(3);
  run(1000, TRUE);
  GC_set_free_space_divisor(1);
  GC_set_on_collection_event(0);
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The manual VDB mode is not built or NO_INCREMENTAL is defined,"
         " test skipped\n");
  return 0;
}
#endif
