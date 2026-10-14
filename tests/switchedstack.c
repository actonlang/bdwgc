/*
 * Copyright (c) 2026 Kristian Larsson
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program for any purpose,
 * provided the above notices are retained on all copies.  Permission to
 * modify the code and to distribute modified code is granted, provided the
 * above notices are retained, and a notice that the code was modified is
 * included with the above copyright notice.
 */

/*
 * Test a mark phase on a global mark stack installed by the switch to the
 * next mark stack.  The procedure set by `GC_set_push_other_roots()`
 * pushes three mark stacks' worth of payloads eagerly, thus the push of
 * the roots overflows the global mark stack several times, its entries
 * are moved to the next mark stack, and the end of the step makes that
 * the global mark stack.  The mark phase then starts on the switched
 * mark stack, in parallel if there are several markers (with
 * `STEAL_MARK_STACK_RANGES`, using the stack as a ring, whose slot
 * numbers are checked in the builds with `GC_ASSERTIONS`).  Each payload
 * refers to a pointer-free leaf.  After each collection the test
 * allocates objects of the sizes of the payloads and the leaves, so that
 * the memory of any reclaimed object is reused, and then checks every
 * leaf.  It also checks that entries were moved and, with several
 * markers, that the collection had a parallel mark phase, which put all
 * the payloads on the global mark stack (with range stealing).
 *
 * This test includes the collector source to make the global mark stack
 * its initial size again before each collection, and to read the moves of
 * the global mark stack entries, the parallel mark phases and the entries
 * put on the global mark stack in them.
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
#if GC_GNUC_PREREQ(4, 6) && defined(MSWIN32) && !defined(NO_DEBUGGING) \
    && !defined(LINT2)
/*
 * In this configuration `ABORT()` returns (after `DebugBreak()`), thus
 * gcc (mingw-w64) may follow a failed assertion of the collector, e.g.
 * that `kind` is less than `MAXOBJKINDS`, and report an index out of the
 * bounds of an array after it.
 */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#  define ARRAY_BOUNDS_IGNORED
#endif
#include "../extra/gc.c"
#ifdef ARRAY_BOUNDS_IGNORED
#  pragma GCC diagnostic pop
#endif
#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
#  pragma GCC diagnostic pop
#endif

#define TEST_ASSERT(e)                                                      \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define CHECK_OUT_OF_MEMORY(p)            \
  do {                                    \
    if (NULL == (p)) {                    \
      fprintf(stderr, "Out of memory\n"); \
      exit(69);                           \
    }                                     \
  } while (0)

/* The largest initial mark stack this test runs with. */
#define MAX_STACK_SIZE ((size_t)1 << 16)

/* The payloads pushed with the roots, in initial mark stack sizes. */
#define ITEMS_PER_STACK 3

#define N_COLLECTIONS 6

#define LEAF_MAGIC ((GC_word)0x5a5a4321UL)

/* The initial size of the global mark stack, in entries. */
static size_t stack_size;

/*
 * The payloads pushed by `push_roots()`, not allocated by the collector,
 * thus marked only by it.
 */
static void ***root_items;
static size_t n_items;

static GC_push_other_roots_proc old_push_other_roots;

/* The moves of the global mark stack entries in a collection. */
static word n_moves;

/* The size of the global mark stack at the end of the marking. */
static size_t size_at_mark_end;

/*
 * The parallel mark phases of a collection, and the entries put on the
 * global mark stack in the last of them (with range stealing).
 */
static word n_phases;
static size_t n_published;

#ifdef PARALLEL_MARK
/* The value of `GC_mark_no` at the start of a collection. */
static word mark_no_at_start;
#endif

#ifdef CHECK_MARK_RING
/* Whether the numbers of the slots were after the global mark stack. */
static GC_bool numbers_follow_stack = TRUE;
#endif

/* A payload refers to a leaf, which holds `LEAF_MAGIC ^ id` and `id`. */
static void **
make_payload(GC_word id)
{
  void **payload = (void **)GC_MALLOC(sizeof(void *));
  GC_word *leaf = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));

  CHECK_OUT_OF_MEMORY(payload);
  CHECK_OUT_OF_MEMORY(leaf);
  leaf[0] = LEAF_MAGIC ^ id;
  leaf[1] = id;
  GC_ptr_store_and_dirty(payload, leaf);
  return payload;
}

/* Return 1 if the leaf of the payload is intact. */
static int
payload_ok(void *const *payload, GC_word id)
{
  const GC_word *leaf;

  if (NULL == GC_base((void *)payload))
    return 0;
  leaf = (const GC_word *)payload[0];
  return GC_base((void *)leaf) != NULL && leaf[0] == (LEAF_MAGIC ^ id)
         && leaf[1] == id;
}

static void GC_CALLBACK
push_roots(void)
{
  if (old_push_other_roots != 0)
    old_push_other_roots();
  /* Push an entry per payload, as the eager root scan does. */
  GC_push_all_eager(root_items, root_items + n_items);
}

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
#ifdef PARALLEL_MARK
  case GC_EVENT_START:
    mark_no_at_start = GC_mark_no;
    break;
#endif
  case GC_EVENT_MARK_END:
    /* `GC_after_marking()`, which clears the counters, is not called yet. */
    n_moves += GC_mark_stack_events.n_moves;
    size_at_mark_end = GC_mark_stack_size;
#ifdef PARALLEL_MARK
    n_phases = GC_mark_no - mark_no_at_start;
#endif
#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES)
    n_published = (size_t)AO_load(&GC_n_published);
#endif
#ifdef CHECK_MARK_RING
    if (GC_mark_ring_numbers != (volatile AO_t *)GC_mark_stack_limit)
      numbers_follow_stack = FALSE;
#endif
    break;
  default:
    break;
  }
}

/* Allocate objects of the sizes of the payloads and the leaves. */
static void
reuse_memory(void)
{
  size_t i;

  for (i = 0; i < 2 * n_items; i++) {
    GC_word *p = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));
    const void *q = GC_MALLOC(sizeof(void *));

    CHECK_OUT_OF_MEMORY(p);
    CHECK_OUT_OF_MEMORY(q);
    p[0] = ~(GC_word)0;
    p[1] = ~(GC_word)0;
  }
}

int
main(void)
{
  size_t i;
  int c;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    return 0;
  }
  stack_size = GC_mark_stack_size;
  if (stack_size > MAX_STACK_SIZE) {
    printf("Initial mark stack of %lu entries is too big for the test\n",
           (unsigned long)stack_size);
    return 0;
  }
  /* Mark in parallel, if available, although no thread is created. */
  GC_start_mark_threads();
  /* Give every mark phase all the markers. */
  GC_set_bytes_per_marker(0);
  GC_set_on_collection_event(on_collection_event);
  n_items = ITEMS_PER_STACK * stack_size;
  root_items = (void ***)malloc(n_items * sizeof(void **));
  CHECK_OUT_OF_MEMORY(root_items);
  /* No collection runs while the payloads are not pushed with the roots. */
  GC_disable();
  for (i = 0; i < n_items; i++)
    root_items[i] = make_payload((GC_word)i);
  old_push_other_roots = GC_get_push_other_roots();
  GC_set_push_other_roots(push_roots);
  GC_enable();
  printf("Initial mark stack of %lu entries, %lu payloads pushed with the"
         " roots, %d markers\n",
         (unsigned long)stack_size, (unsigned long)n_items,
         GC_get_parallel() + 1);

  for (c = 1; c <= N_COLLECTIONS; c++) {
    size_t n_bad = 0;

    /* Make the mark stack its initial size, and recycle the larger one. */
    LOCK();
    alloc_mark_stack(stack_size);
    GC_after_marking();
    UNLOCK();
    n_moves = 0;
    GC_gcollect();
    /* No collection runs until the payloads are checked. */
    GC_disable();
    reuse_memory();
    for (i = 0; i < n_items; i++) {
      if (!payload_ok(root_items[i], (GC_word)i))
        n_bad++;
    }
    GC_enable();
    printf("Collection %d: %lu payloads reclaimed, %lu moves, mark stack of"
           " %lu entries after the marking, %lu parallel mark phases, the"
           " last put %lu entries on the global mark stack\n",
           c, (unsigned long)n_bad, (unsigned long)n_moves,
           (unsigned long)size_at_mark_end, (unsigned long)n_phases,
           (unsigned long)n_published);
    TEST_ASSERT(0 == n_bad);
    TEST_ASSERT(n_moves > 0);
    TEST_ASSERT(size_at_mark_end > stack_size);
#ifdef CHECK_MARK_RING
    TEST_ASSERT(numbers_follow_stack);
#endif
    if (GC_get_parallel() > 0) {
      TEST_ASSERT(n_phases > 0);
#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES)
      /* The phase starts with the payloads on the switched mark stack. */
      TEST_ASSERT(n_published >= n_items);
#endif
    }
  }
  printf("SUCCEEDED\n");
  return 0;
}
