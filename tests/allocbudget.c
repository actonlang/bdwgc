/*
 * Test the heap sizing policy knobs: the allocation volume between
 * collections proportional to the size of the live data.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#ifndef GC_IGNORE_WARN
/* Ignore misleading "Out of Memory!" warnings. */
#  define GC_IGNORE_WARN
#endif

#include "gc.h"

/* Total size of the live objects (nodes) in bytes. */
#ifndef LIVE_BYTES
#  define LIVE_BYTES (32 << 20)
#endif

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

struct node_s {
  struct node_s *next;
  GC_word value;
};

static struct node_s *volatile live_list;

/* Size of the pointer-free live data, and of its chunks, in bytes. */
#ifndef ATOMIC_LIVE_BYTES
#  define ATOMIC_LIVE_BYTES (8 << 20)
#endif
#define ATOMIC_CHUNK_BYTES 1000

static void **volatile atomic_chunks;

static struct node_s *
new_node(struct node_s *next, GC_word value)
{
  struct node_s *p = GC_NEW(struct node_s);

  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  p->next = next;
  p->value = value;
  return p;
}

static void *
checked_malloc_atomic(size_t lb)
{
  void *p = GC_MALLOC_ATOMIC(lb);

  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}

/* Return the heap size including the unmapped part. */
static size_t
total_heap_size(void)
{
  return GC_get_heap_size() + GC_get_unmapped_bytes();
}

/* Allocate `n` unreachable nodes. */
static void
alloc_garbage(size_t n)
{
  struct node_s *volatile p = NULL;
  size_t i;

  for (i = 0; i < n; i++)
    p = new_node(NULL, (GC_word)i);
  (void)p;
}

int
main(void)
{
  size_t i;
  size_t n_live = LIVE_BYTES / sizeof(struct node_s);
  GC_word gc_no, heap_growth_divisor;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  /*
   * Build pointer-free live data, which contributes little to the volume
   * derived from `GC_free_space_divisor`.  With the allocation volume
   * equal to the size of the live data, allocating a quarter as much
   * should not cause a collection (unless the incremental mode is on).
   * Note: a second heap expansion before the next collection triggers
   * the collection (to keep the limits used by the black-listing), thus
   * the allocated amount should fit in the free space of the heap plus
   * a single expansion.  The latter is about a third of the heap size
   * (by default) if the heap growth divisor is set, otherwise it is
   * limited to a fixed number of heap blocks (e.g. 1 MB if `HBLKSIZE` is
   * 512).
   */
  atomic_chunks = (void **)GC_MALLOC(
      sizeof(void *) * (ATOMIC_LIVE_BYTES / ATOMIC_CHUNK_BYTES));
  CHECK(atomic_chunks != NULL);
  for (i = 0; i < ATOMIC_LIVE_BYTES / ATOMIC_CHUNK_BYTES; i++)
    atomic_chunks[i] = checked_malloc_atomic(ATOMIC_CHUNK_BYTES);
  heap_growth_divisor = GC_get_heap_growth_divisor();
  GC_set_heap_growth_divisor(1);
  GC_set_alloc_budget_percent(100);
  CHECK(GC_get_alloc_budget_percent() == 100);
  GC_gcollect();
  gc_no = GC_get_gc_no();
  for (i = 0; i < ATOMIC_LIVE_BYTES / ATOMIC_CHUNK_BYTES / 4; i++)
    (void)checked_malloc_atomic(ATOMIC_CHUNK_BYTES);
  printf("Collections (atomic live data): %lu\n",
         (unsigned long)(GC_get_gc_no() - gc_no));
  if (!GC_is_incremental_mode() && !GC_is_disabled())
    CHECK(GC_get_gc_no() == gc_no);
  GC_set_alloc_budget_percent(0);
  CHECK(0 == GC_get_alloc_budget_percent());
  GC_set_heap_growth_divisor(heap_growth_divisor);
  atomic_chunks = NULL;

  for (i = 0; i < n_live; i++)
    live_list = new_node(live_list, (GC_word)i);

  if (!GC_is_disabled()) {
    size_t heap_sz;

    /*
     * The heap size limit is respected.  With a big allocation volume,
     * the limit is reached before the volume is, and the collector
     * should collect instead of failing (even if no retries are allowed).
     */
    GC_set_free_space_divisor(3);
    GC_set_alloc_budget_percent(1000);
    GC_gcollect();
    heap_sz = total_heap_size();
    GC_set_max_heap_size(heap_sz + LIVE_BYTES / 4);
    GC_set_max_retries(0);
    alloc_garbage(8 * n_live);
    /*
     * In the incremental mode, the heap might also grow by the space of
     * the old mark stack when the mark stack is enlarged, and the limit
     * does not apply to the recycled space.  E.g., with the soft-dirty
     * bits, a collection following a heap expansion finds every page of
     * the heap dirty, and pushes all the marked objects of the heap.
     */
    if (!GC_is_incremental_mode())
      CHECK(total_heap_size() <= heap_sz + LIVE_BYTES / 4);
    GC_set_alloc_budget_percent(0);
  }

  for (i = 0; live_list != NULL; i++)
    live_list = live_list->next;
  CHECK(i == n_live);
  printf("SUCCEEDED\n");
  return 0;
}
