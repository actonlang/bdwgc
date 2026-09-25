/*
 * Test that an allocation does not fail if the heap size limit is
 * reached before the allocation volume which triggers the next
 * collection, while most of the heap is garbage.
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

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  for (i = 0; i < n_live; i++)
    live_list = new_node(live_list, (GC_word)i);

  if (!GC_is_disabled()) {
    size_t heap_sz;

    /*
     * The heap size limit is respected.  The allocation volume between
     * collections is set to 10 times the size of the live data, thus
     * the limit is reached before the volume is, and the collector
     * should collect instead of failing (even if no retries are allowed).
     */
    GC_set_min_bytes_allocd((size_t)10 * LIVE_BYTES);
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
    printf("Heap size: %lu KiB (limit: %lu KiB), collections: %lu\n",
           (unsigned long)(total_heap_size() >> 10),
           (unsigned long)((heap_sz + LIVE_BYTES / 4) >> 10),
           (unsigned long)GC_get_gc_no());
  }

  for (i = 0; live_list != NULL; i++)
    live_list = live_list->next;
  CHECK(i == n_live);
  printf("SUCCEEDED\n");
  return 0;
}
