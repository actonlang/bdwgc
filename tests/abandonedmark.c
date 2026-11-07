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
 * Test that the heap does not grow while a marking that a stop function
 * abandoned is in progress, in the incremental mode without a time limit
 * (`GC_TIME_UNLIMITED`), where a collection is marked in one world-stopped
 * mark: the first shortage of heap blocks completes the marking.  The test
 * builds a list of `N_NODES` nodes, collects, writes to every node (so that
 * the minor collection that follows rescans every block of the list), sets
 * the marking rate (`GC_set_rate()`) to 1 and starts a collection whose
 * world-stopped mark the stop function abandons.  Then it allocates
 * `GARBAGE_BYTES` of small objects, which it drops.  The marking needs
 * about one unit of marking work per block of the list, and the allocation
 * does at most one per block it takes, thus the marking completes only
 * after about the bytes of the list are allocated, more than the free
 * space the collection before left.  The test fails if the heap grew
 * before the end of that marking (`GC_EVENT_MARK_END`), or if the marking
 * did not end.  The test is skipped while the collections are disabled
 * (e.g. by `GC_DONT_GC`), and without an incremental mode with an
 * automatic VDB: the manual VDB mode marks in world-stopped steps where the
 * collector is built with the parallel marker, and otherwise in one
 * world-stopped mark that no stop function abandons.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/* The nodes of the list (of 4 words each, 16 MiB with 64-bit words). */
#ifndef N_NODES
#  define N_NODES ((size_t)512 * 1024)
#endif

/* The bytes of the dropped objects allocated after the abandoned mark. */
#ifndef GARBAGE_BYTES
#  define GARBAGE_BYTES ((size_t)64 << 20)
#endif

#define GARBAGE_OBJ_BYTES 32

struct node {
  struct node *next;
  GC_word value[3];
};

static struct node *list;

/*
 * Whether the stop function abandons the next world-stopped mark, whether
 * a marking completed since the abandoned mark, and the heap growths
 * before that.  The collector calls the stop function and the callbacks
 * with the allocator lock held, thus the lock protects these.  The last
 * two are updated by the callbacks during an allocation call, thus
 * `volatile`: the compiler may assume that the allocation functions do not
 * modify any global variable (`GC_ATTR_MALLOC` includes
 * `__declspec(noalias)` for MS VC, and for clang targeting MS VC), and
 * without threads `GC_alloc_lock()` and `GC_alloc_unlock()` expand to
 * nothing.
 */
static int abandon_next = 0;
static volatile int marking_done = 0;
static volatile unsigned growths_before_mark_end = 0;

static int GC_CALLBACK
stop_func(void)
{
  if (abandon_next) {
    abandon_next = 0;
    return 1;
  }
  return 0;
}

static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_MARK_END == e)
    marking_done = 1;
}

static void GC_CALLBACK
on_heap_resize(GC_word new_size)
{
  (void)new_size;
  if (!marking_done)
    growths_before_mark_end++;
}

int
main(void)
{
  unsigned vdb;
  size_t i;
  struct node *n;
  int done;
  unsigned growths;

  GC_INIT();
  if (GC_is_disabled()) {
    printf("The collections are disabled, test skipped\n");
    return 0;
  }
#ifndef NO_INCREMENTAL
  GC_enable_incremental();
#endif
  vdb = GC_get_actual_vdb();
  if (GC_VDB_NONE == vdb || (vdb & GC_VDB_MANUAL) != 0) {
    printf("The incremental mode with an automatic VDB is unavailable,"
           " test skipped\n");
    return 0;
  }
  GC_set_time_limit(GC_TIME_UNLIMITED);
  for (i = 0; i < N_NODES; i++) {
    n = GC_NEW(struct node);
    CHECK(n != NULL);
    n->next = list;
    list = n;
  }
  GC_gcollect();
  for (n = list; n != NULL; n = n->next)
    n->value[0]++;
  GC_set_rate(1);
  GC_set_stop_func(stop_func);
  GC_set_on_heap_resize(on_heap_resize);
  GC_set_on_collection_event(on_event);
  GC_alloc_lock();
  abandon_next = 1;
  GC_alloc_unlock();
  GC_start_incremental_collection();
  GC_alloc_lock();
  CHECK(0 == abandon_next);
  CHECK(!marking_done);
  GC_alloc_unlock();
  for (i = 0; i < GARBAGE_BYTES / GARBAGE_OBJ_BYTES; i++)
    CHECK(GC_MALLOC(GARBAGE_OBJ_BYTES) != NULL);
  GC_alloc_lock();
  done = marking_done;
  growths = growths_before_mark_end;
  GC_alloc_unlock();
  GC_set_on_collection_event(0);
  GC_set_on_heap_resize(0);
  printf("Heap growths before the end of the abandoned marking: %u\n",
         growths);
  CHECK(done);
  if (growths > 0) {
    fprintf(stderr,
            "The heap grew %u times while the abandoned marking was in"
            " progress\n",
            growths);
    exit(1);
  }
  for (n = list, i = 0; n != NULL; n = n->next)
    i++;
  CHECK(N_NODES == i);
  printf("SUCCEEDED\n");
  return 0;
}
