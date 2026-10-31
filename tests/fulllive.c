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
 * Test which collections of the incremental mode are full when the old
 * objects die: the manual VDB mode without a time limit, no full
 * collection by the count of minor ones, and a free space divisor of 1,
 * so that the growth of the heap does not make a collection full.  The
 * client keeps a live tree and a ring of its latest objects, and replaces
 * the objects of the ring before each collection.  A minor collection
 * marks the objects in the ring, which die at the next refill but stay
 * marked until a full collection, thus the bytes in use after each minor
 * collection grow.  After a full collection, the minor collections go on
 * until the bytes in use exceed those after it by more than half, and no
 * collection may be full.  This test includes the collector source to read
 * the bytes in use and the state of the heap-growth trigger.  Skipped
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

/* The nodes of the live tree, of 4 words each (4 MiB). */
#  define N_NODES (1 << 17)

/* The objects in the ring (256 KiB of nodes). */
#  define N_RING (1 << 13)

/* The collections recorded at most. */
#  define MAX_COLLECTIONS 400

/* The minor collections over the bound to run. */
#  define N_OVER 3

struct node {
  struct node *left;
  struct node *right;
  GC_word value;
  GC_word pad;
};

static struct node *volatile root;
static struct node **volatile ring;

/*
 * A collection: whether it was full, the bytes in use after it, and
 * whether the growth of the heap made the next collection full.
 */
struct collection {
  GC_bool full;
  GC_bool heap_growth;
  word in_use;
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

/*
 * Check the collections recorded from the full collection `first` to
 * `n`: none but the first one is full, and the growth of the heap never
 * made a collection full.  Return the number of minor collections whose
 * bytes in use exceed those after the full one by more than half.
 */
static unsigned
check_run(unsigned first, unsigned n)
{
  word bound = colls[first].in_use + colls[first].in_use / 2;
  unsigned i;
  unsigned n_over = 0;

  CHECK(colls[first].full);
  for (i = first + 1; i < n; i++) {
    CHECK(!colls[i - 1].heap_growth);
    CHECK(!colls[i].full);
    if (colls[i].in_use > bound)
      n_over++;
  }
  return n_over;
}

int
main(void)
{
  unsigned first, n;
  GC_word value = 0;

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
  GC_gcollect();
  first = locked_n_colls() - 1;
  do {
    refill(++value);
    GC_start_incremental_collection();
    n = locked_n_colls();
    CHECK(n < MAX_COLLECTIONS);
  } while (check_run(first, n) < N_OVER);
  GC_set_on_collection_event(0);
  printf("%u collections, bytes in use after the full one %lu KiB, after"
         " the last one %lu KiB\n",
         n - first, (unsigned long)(colls[first].in_use >> 10),
         (unsigned long)(colls[n - 1].in_use >> 10));
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
