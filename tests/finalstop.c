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
 * Test that collections complete when the client's stop function stops
 * every world-stopped mark as early as it can: the incremental mode with
 * the manual VDB and a time limit of 1 ms, a live list of about 1 MiB,
 * and a stop function that returns true during every world-stopped mark.
 * The world-stopped marks of a collection are then abandoned after their
 * roots are pushed, however long their time limit, until a final mark runs
 * without the time limit, and steps mark in between.  The stop function
 * lets a step mark until its deadline: with one `GC_mark_some()` per step,
 * the steps would fall behind the allocation in a program with megabytes
 * of static data, e.g. one linked with a sanitizer runtime, whose roots
 * every world-stopped mark pushes again.  `N_COLLECTIONS` collections must
 * complete before the heap grows beyond `MAX_HEAP`.  Without the manual
 * VDB mode, or if `NO_INCREMENTAL` is defined, the test is skipped.
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

#define MiB ((size_t)1 << 20)

/* The nodes of the live list, of 4 words each. */
#define N_NODES (32 * 1024)

/* The collections to complete, and the heap size not to exceed meanwhile. */
#define N_COLLECTIONS 5
#define MAX_HEAP (512 * MiB)

struct node {
  struct node *next;
  GC_word value;
  GC_word pad[2];
};

static struct node *volatile live;
static void *volatile junk;

/*
 * Whether the world was stopped for a world-stopped mark (not for a step).
 * Updated by `on_collection_event()`, which the collector calls holding the
 * allocator lock on the thread that collects.
 */
static volatile int marking;

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
  case GC_EVENT_POST_STOP_WORLD:
  case GC_EVENT_POST_START_WORLD:
    marking = 0;
    break;
  case GC_EVENT_MARK_START:
    marking = 1;
    break;
  default:
    break;
  }
}

static int GC_CALLBACK
stop_marks(void)
{
  return marking;
}

int
main(void)
{
  GC_stop_func old_stop_func;
  GC_word start;
  unsigned long i;

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
  GC_set_time_limit(1);
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
  GC_set_stop_func(stop_marks);
  start = GC_get_gc_no();
  for (i = 0; GC_get_gc_no() - start < N_COLLECTIONS; i++) {
    junk = GC_MALLOC(sizeof(struct node));
    CHECK(junk != NULL);
    if (i % 1024 == 0 && GC_get_heap_size() > MAX_HEAP) {
      fprintf(stderr, "Heap %lu MiB after %lu collections\n",
              (unsigned long)(GC_get_heap_size() / MiB),
              (unsigned long)(GC_get_gc_no() - start));
      CHECK(0);
    }
  }
  GC_set_stop_func(old_stop_func);
  printf("%d collections, heap %lu MiB\n", N_COLLECTIONS,
         (unsigned long)(GC_get_heap_size() / MiB));
  printf("SUCCEEDED\n");
  return 0;
}
