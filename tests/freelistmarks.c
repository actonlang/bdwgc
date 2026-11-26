/*
 * Copyright (c) 2026 Acton contributors
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
 * Test that a collection with parallel markers frees a block whose only
 * marked objects are on a free list.  With the collections disabled, the
 * test allocates objects of one size by `GC_generic_malloc()`, thus not
 * from a thread-local free list (whose objects every collection marks),
 * until it holds every object of a heap block.  Then it frees all of them
 * explicitly, which links them on a free list, while a static variable
 * keeps a pointer to the middle object of that block, as a stale pointer
 * of a client would.  The collection marks that object and, through the
 * links, the objects freed before it.  The finish of the collection
 * clears the mark bits of the objects on the free lists, which decrements
 * the mark counts of their blocks.  With parallel markers, the collector
 * takes the mark counts for approximate, thus it stores a count that the
 * decrements bring to zero, which means a block without marked objects,
 * only if no mark bit of the block is set.  The test fails if the block is
 * still in use after the collection, and then prints how many of its
 * objects are marked.  The collecting thread marks alone, as the test sets
 * the bytes per marker (`GC_set_bytes_per_marker()`) to the maximum: two
 * markers which mark an object at once may count it twice, and then the
 * count of its block stays above zero although no object of the block is
 * marked.  The test is skipped with fewer than two markers (in a build
 * without threads or without the parallel marker, or with one marker), in
 * the leak detection mode, and while the collections are disabled.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"
#include "gc/gc_inline.h"
#include "gc/gc_mark.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/* The requested size of the objects. */
#define OBJ_BYTES 200

/* The largest heap block size of the collector. */
#define MAX_HBLK_BYTES 65536

/*
 * The objects allocated at most: those of two heap blocks of the largest
 * size, the first of which the allocation may find partly used.
 */
#define MAX_OBJS (2 * (MAX_HBLK_BYTES / OBJ_BYTES))

/*
 * The hidden pointers to the objects, thus not roots.  The collections
 * are disabled while the objects are allocated.
 */
static GC_hidden_pointer objs[MAX_OBJS];

/* The objects allocated. */
static size_t n_objs;

/* The size of each object, and the number of the objects of a block. */
static size_t obj_bytes;
static size_t block_objs;

/* The pointer to the middle object of the block. */
static void *volatile keep;

/*
 * Allocate objects until the latest `block_objs` ones are all the objects
 * of one heap block, keep a pointer to the middle one of these, and free
 * every object allocated.
 */
static void
make_free_list(void)
{
  size_t hblk_bytes = GC_get_hblk_size();
  GC_word last_block = 0;
  size_t i, n_in_block = 0;

  CHECK(hblk_bytes <= MAX_HBLK_BYTES);
  GC_disable();
  do {
    void *p;
    GC_word block;

    if (n_objs == MAX_OBJS) {
      fprintf(stderr, "No heap block filled with the first %d objects\n",
              MAX_OBJS);
      exit(1);
    }
    p = GC_generic_malloc(OBJ_BYTES, GC_I_NORMAL);
    CHECK(p != NULL);
    if (0 == n_objs) {
      obj_bytes = GC_size(p);
      block_objs = hblk_bytes / obj_bytes;
    }
    objs[n_objs++] = GC_HIDE_POINTER(p);
    block = (GC_word)(GC_uintptr_t)p & ~(GC_word)(hblk_bytes - 1);
    n_in_block = block == last_block ? n_in_block + 1 : 1;
    last_block = block;
  } while (n_in_block < block_objs);
  keep = GC_REVEAL_POINTER(objs[n_objs - block_objs + block_objs / 2]);
  for (i = 0; i < n_objs; i++)
    GC_FREE(GC_REVEAL_POINTER(objs[i]));
  GC_enable();
}

/* Count the marked objects of the block of `keep`, which is in use. */
static void *GC_CALLBACK
count_marked(void *pn_marked)
{
  GC_word block
      = (GC_word)(GC_uintptr_t)keep & ~(GC_word)(GC_get_hblk_size() - 1);
  size_t i;

  for (i = 0; i < block_objs; i++) {
    if (GC_is_marked((void *)(GC_uintptr_t)(block + i * obj_bytes)))
      ++*(unsigned *)pn_marked;
  }
  return NULL;
}

int
main(void)
{
  GC_INIT();
  if (GC_get_find_leak()) {
    printf("The leak detection mode is on, test skipped\n");
    return 0;
  }
  if (GC_is_disabled()) {
    printf("The collections are disabled, test skipped\n");
    return 0;
  }
  /* Start the markers, although the test creates no thread. */
  GC_start_mark_threads();
  if (0 == GC_get_parallel()) {
    printf("Fewer than two markers, test skipped\n");
    return 0;
  }
  GC_set_bytes_per_marker(~(size_t)0);

  make_free_list();
  GC_gcollect();
  if (GC_base((void *)keep) != NULL) {
    unsigned n_marked = 0;

    (void)GC_call_with_alloc_lock(count_marked, &n_marked);
    fprintf(stderr,
            "The block of the freed objects is in use after the collection,"
            " with %u of its %lu objects marked\n",
            n_marked, (unsigned long)block_objs);
    exit(1);
  }
  printf("SUCCEEDED (a block of %lu objects of %lu bytes freed, %d markers,"
         " %lu objects freed in all)\n",
         (unsigned long)block_objs, (unsigned long)obj_bytes,
         GC_get_parallel() + 1, (unsigned long)n_objs);
  return 0;
}
