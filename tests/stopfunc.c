/*
 * Test that collections with a custom stop function (which disables the
 * parallel marker, if any) recover from a mark stack overflow by growing
 * the mark stack, i.e. the overflow should not recur in every collection.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"

#ifndef N_NODES
/*
 * The number of nodes in the graph.  Marking the graph requires a mark
 * stack of much more than `INITIAL_MARK_STACK_SIZE` entries.
 */
#  define N_NODES (256 * 1024)
#endif

#ifndef N_COLLECTIONS
/* Enough for the mark stack to grow to the required size. */
#  define N_COLLECTIONS 12
#endif

/* The number of random edges per node (a chain edge is extra). */
#define N_EDGES 3

#define TEST_ASSERT(e)                                                    \
  if (!(e)) {                                                             \
    fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
            #e);                                                          \
    exit(1);                                                              \
  }

#define CHECK_OUT_OF_MEMORY(p)            \
  do {                                    \
    if (NULL == (p)) {                    \
      fprintf(stderr, "Out of memory\n"); \
      exit(69);                           \
    }                                     \
  } while (0)

struct node_s {
  struct node_s *edges[N_EDGES];
  struct node_s *next;
};

static struct node_s *root;
static struct node_s **nodes;

static unsigned long stop_func_calls;

static int GC_CALLBACK
never_stop_func(void)
{
  stop_func_calls++;
  return 0;
}

/*
 * The stop function is called also before the marking, once per block
 * swept by `GC_reclaim_all()`; in the find-leak mode, the blocks full of
 * live objects are swept too.  Count only the calls during the marking.
 */
static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  if (GC_EVENT_MARK_START == e)
    stop_func_calls = 0;
}

/*
 * Collect with a stop function which never aborts the collection, and
 * return the number of its calls during the marking, i.e. roughly the
 * number of the marker steps.
 */
static unsigned long
collect_with_stop_func(void)
{
  stop_func_calls = 0;
  TEST_ASSERT(GC_try_to_collect(never_stop_func));
  return stop_func_calls;
}

static unsigned rand_state = 1;

static unsigned
next_random(void)
{
  rand_state = rand_state * 1103515245U + 12345U;
  return rand_state >> 8;
}

int
main(void)
{
  size_t i;
  int j;
  unsigned long list_calls, calls = 0;
  const struct node_s *p;

  GC_INIT();
  GC_set_on_collection_event(on_collection_event);
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  if (GC_is_disabled()) {
    /* E.g., `GC_DONT_GC` environment variable is set. */
    printf("Skipping the test as the collector is disabled\n");
    return 0;
  }
  /* Test with the parallel marker, if it is enabled. */
  GC_start_mark_threads();

  nodes = (struct node_s **)GC_MALLOC(N_NODES * sizeof(struct node_s *));
  CHECK_OUT_OF_MEMORY(nodes);
  for (i = 0; i < N_NODES; i++) {
    p = GC_NEW(struct node_s);
    CHECK_OUT_OF_MEMORY(p);
    GC_PTR_STORE_AND_DIRTY(&nodes[i], p);
  }
  for (i = 0; i + 1 < N_NODES; i++)
    GC_PTR_STORE_AND_DIRTY(&nodes[i]->next, nodes[i + 1]);
  root = nodes[0];
  nodes = NULL;

  /*
   * A long list is marked without a mark stack overflow.  Its marking
   * work is used as a reference.
   */
  list_calls = collect_with_stop_func();

  nodes = (struct node_s **)GC_MALLOC(N_NODES * sizeof(struct node_s *));
  CHECK_OUT_OF_MEMORY(nodes);
  for (i = 0, p = root; i < N_NODES; i++, p = p->next)
    GC_PTR_STORE_AND_DIRTY(&nodes[i], p);
  for (i = 0; i < N_NODES; i++) {
    for (j = 0; j < N_EDGES; j++)
      GC_PTR_STORE_AND_DIRTY(&nodes[i]->edges[j],
                             nodes[next_random() % N_NODES]);
  }
  nodes = NULL;

  /*
   * A random graph (of the same nodes) overflows the initial mark stack.
   * After the mark stack is grown enough, marking should not take much
   * more steps than marking the list.
   */
  for (j = 0; j < N_COLLECTIONS; j++) {
    calls = collect_with_stop_func();
  }
  printf("Stop function calls: %lu (list), %lu (graph, last collection)\n",
         list_calls, calls);
  TEST_ASSERT(calls < 2 * list_calls);
  for (i = 0, p = root; i < N_NODES; i++, p = p->next)
    TEST_ASSERT(p != NULL);
  printf("SUCCEEDED\n");
  return 0;
}
