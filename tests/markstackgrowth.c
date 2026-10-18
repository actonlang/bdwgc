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
 * Test that the collector keeps every reachable object of heap shapes that
 * need many more entries of the global mark stack than it has initially
 * (`INITIAL_MARK_STACK_SIZE` entries if the macro is defined, otherwise
 * `HBLKSIZE` entries).
 *
 * The first shape is a list whose nodes refer to a payload ahead of the
 * next node, thus marking the list leaves an entry on the mark stack per
 * node, and in a parallel mark phase the marker that walks the list keeps
 * copying entries to the global mark stack while the other markers take
 * them.  Each payload refers to a pointer-free leaf.  The second shape is
 * a random graph whose nodes refer to several others, thus many entries
 * wait on the mark stack at a time.  After each collection, the test
 * allocates objects of the sizes of the objects of these shapes, so that
 * the memory of any object the collection reclaimed is reused, and then
 * checks every object of both shapes.
 *
 * This test includes the collector source to print the number of the
 * local mark stacks dropped in each collection because the global mark
 * stack had no room for them.  With `STEAL_MARK_STACK_RANGES`, a parallel
 * mark phase uses the global mark stack as a ring, thus it can put more
 * entries on the global mark stack than it has slots.  To check that it
 * does, the test then makes the global mark stack its initial size again,
 * but at least as long as two local mark stacks, breaks up the graph and
 * collects with the list as the only live shape.  If the stack is at most
 * half as long as the list, then this collection should drop no local mark
 * stack and its parallel mark phase, if any (none with one marker), should
 * put more entries on the global mark stack than it has slots.  A fallback
 * to dropping local mark stacks instead of reusing the slots fails this.
 *
 * With `STEAL_MARK_STACK_RANGES`, a parallel mark phase also grows the
 * ring instead of dropping a local mark stack for which it has no room,
 * thus no collection should drop one.  The test checks this, and then
 * makes the global mark stack its initial size again and refuses the
 * first request of memory from the OS in a collection (the test includes
 * the collector source for this).  If the collection marks in parallel
 * and a marker has no room on the ring, then this is the request of the
 * growth of the ring, and the local mark stacks without room are dropped;
 * otherwise it is the request of a larger mark stack in serial marking,
 * and entries of the global mark stack are dropped.  Either way, the
 * marking is completed from the marked objects.
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

/* Whether to refuse the next request of memory from the OS. */
static GC_bool refuse_next_get_mem;

/* The number of the requests refused. */
static unsigned n_refused;

static ptr_t
get_test_memory(size_t bytes)
{
  if (refuse_next_get_mem) {
    refuse_next_get_mem = FALSE;
    n_refused++;
    return NULL;
  }
  return (ptr_t)GET_MEM(bytes);
}

#undef GET_MEM
#define GET_MEM(bytes) get_test_memory(bytes)

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

/* The length of the list, in initial mark stack sizes, and its maximum. */
#define LIST_LEN_PER_STACK 64
#define MAX_LIST_LEN ((size_t)1 << 20)

/* The nodes of the graph, in initial mark stack sizes, and their maximum. */
#define GRAPH_NODES_PER_STACK 16
#define MAX_GRAPH_NODES ((size_t)1 << 19)

/* The number of the references of each node of the graph. */
#define GRAPH_DEGREE 4

#define N_COLLECTIONS 4

#define LEAF_MAGIC ((GC_word)0x5a5a4321UL)

struct graph_node_s {
  GC_word id;
  struct graph_node_s *refs[GRAPH_DEGREE];
};

static size_t list_len;
static size_t graph_nodes;

/*
 * The roots of the shapes, the first node of the list and the first node
 * of the graph (every node is reachable from it).  These are volatile:
 * the compiler may make a static only used by `main()` a local variable
 * of it (in C++, where `main()` cannot be called recursively), and drop
 * one which is only written, as `graph` is.
 */
static void **volatile list;
static struct graph_node_s *volatile graph;

/*
 * The nodes of the graph, by their number, as hidden pointers in
 * a pointer-free object, so that the table keeps no node reachable.
 */
static GC_hidden_pointer *graph_table;

/*
 * The local mark stacks dropped by the markers in a collection because
 * the global mark stack had no room for them.
 */
static word n_no_room;

#ifdef GROW_MARK_RING
/* The growths of the ring during the parallel mark phases of a collection. */
static word n_ring_grown;
#endif

/* The overflows of the global mark stack which dropped entries. */
static word n_overflows;

#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES)
#  define CHECK_RING

/* The value of `GC_mark_no` at the start of a collection. */
static word mark_no_at_start;

/*
 * The parallel mark phases of a collection, the entries put on the global
 * mark stack in the last of them, and the size of the global mark stack
 * then.
 */
static word n_phases;
static size_t n_ring_entries;
static size_t ring_size;
#endif

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
#ifdef CHECK_RING
  case GC_EVENT_START:
    mark_no_at_start = GC_mark_no;
    break;
#endif
  case GC_EVENT_MARK_END:
    /* `GC_after_marking()`, which clears the counter, is not called yet. */
    n_no_room += GC_mark_stack_events.n_no_room;
    n_overflows += GC_mark_stack_events.n_overflows;
#ifdef GROW_MARK_RING
    n_ring_grown += GC_mark_stack_events.n_ring_grown;
#endif
#ifdef CHECK_RING
    n_phases = GC_mark_no - mark_no_at_start;
    n_ring_entries = (size_t)AO_load(&GC_n_published);
    ring_size = GC_mark_stack_size;
#endif
    break;
  default:
    break;
  }
}

/*
 * The node referred to by reference `k` of node `i` of the graph.
 * Reference 0 is to the next node, so that every node is reachable from
 * the first one; the others are to pseudo-random nodes.
 */
static size_t
graph_ref(size_t i, unsigned k)
{
  GC_word x;

  if (0 == k)
    return (i + 1) % graph_nodes;
  x = (GC_word)i * GRAPH_DEGREE + k;
  x ^= x >> 13;
  x *= (GC_word)0x9e3779b1UL;
  x ^= x >> 16;
  return (size_t)(x % graph_nodes);
}

static void
make_list(void)
{
  size_t i;

  for (i = list_len; i > 0; i--) {
    void **node = (void **)GC_MALLOC(2 * sizeof(void *));
    void **payload = (void **)GC_MALLOC(sizeof(void *));
    GC_word *leaf = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));

    CHECK_OUT_OF_MEMORY(node);
    CHECK_OUT_OF_MEMORY(payload);
    CHECK_OUT_OF_MEMORY(leaf);
    leaf[0] = LEAF_MAGIC ^ (GC_word)(i - 1);
    leaf[1] = (GC_word)(i - 1);
    GC_ptr_store_and_dirty(payload, leaf);
    GC_ptr_store_and_dirty(&node[0], payload);
    GC_ptr_store_and_dirty(&node[1], list);
    list = node;
  }
}

static void
make_graph(void)
{
  size_t i;
  unsigned k;

  graph_table = (GC_hidden_pointer *)GC_MALLOC_ATOMIC(
      graph_nodes * sizeof(GC_hidden_pointer));
  CHECK_OUT_OF_MEMORY(graph_table);
  for (i = 0; i < graph_nodes; i++) {
    struct graph_node_s *node = GC_NEW(struct graph_node_s);

    CHECK_OUT_OF_MEMORY(node);
    node->id = (GC_word)i;
    graph_table[i] = GC_HIDE_POINTER(node);
  }
  for (i = 0; i < graph_nodes; i++) {
    struct graph_node_s *node
        = (struct graph_node_s *)GC_REVEAL_POINTER(graph_table[i]);

    for (k = 0; k < GRAPH_DEGREE; k++) {
      GC_ptr_store_and_dirty(&node->refs[k],
                             GC_REVEAL_POINTER(graph_table[graph_ref(i, k)]));
    }
  }
  graph = (struct graph_node_s *)GC_REVEAL_POINTER(graph_table[0]);
}

/*
 * Allocate and fill objects of the sizes and kinds of the objects of the
 * list and of the graph, twice as many as there are of each, so that the
 * blocks of these objects are swept and any reclaimed object is reused.
 */
static void
reuse_memory(void)
{
  size_t i;

  for (i = 0; i < 2 * list_len; i++) {
    GC_word *leaf = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));
    void **node = (void **)GC_MALLOC(2 * sizeof(void *));
    void **payload = (void **)GC_MALLOC(sizeof(void *));

    CHECK_OUT_OF_MEMORY(leaf);
    CHECK_OUT_OF_MEMORY(node);
    CHECK_OUT_OF_MEMORY(payload);
    leaf[0] = ~(GC_word)0;
    leaf[1] = ~(GC_word)0;
  }
  for (i = 0; i < 2 * graph_nodes; i++) {
    struct graph_node_s *node = GC_NEW(struct graph_node_s);

    CHECK_OUT_OF_MEMORY(node);
    node->id = ~(GC_word)0;
  }
}

/*
 * Return 1 if `p` is the address that the allocation calls of this test
 * return for an object of the heap: the start of the object, or the
 * address after its debug header if `GC_DEBUG` is defined (the macros
 * allocate by the debug calls then).
 */
static int
is_object(const void *p)
{
  void *base = GC_base((void *)p);

  if (NULL == base)
    return 0;
#ifdef GC_DEBUG
  return GC_USR_PTR_FROM_BASE(base) == p;
#else
  return base == p;
#endif
}

/*
 * Return the number of the nodes of the list whose objects were found
 * reclaimed; the rest of the list from the first node found reclaimed
 * cannot be followed, and counts as reclaimed.
 */
static size_t
check_list(void)
{
  void **node = list;
  size_t i;

  for (i = 0; i < list_len; i++) {
    void **payload;
    const GC_word *leaf;

    if (NULL == node || !is_object(node))
      return list_len - i;
    payload = (void **)node[0];
    if (NULL == payload || !is_object(payload))
      return list_len - i;
    leaf = (const GC_word *)payload[0];
    if (NULL == leaf || !is_object(leaf) || leaf[0] != (LEAF_MAGIC ^ i)
        || leaf[1] != i)
      return list_len - i;
    node = (void **)node[1];
  }
  TEST_ASSERT(NULL == node);
  return 0;
}

/* Return the number of the nodes of the graph found reclaimed. */
static size_t
check_graph(void)
{
  size_t n_bad = 0;
  size_t i;
  unsigned k;

  for (i = 0; i < graph_nodes; i++) {
    const struct graph_node_s *node
        = (const struct graph_node_s *)GC_REVEAL_POINTER(graph_table[i]);
    int ok = is_object(node) && node->id == (GC_word)i;

    for (k = 0; ok && k < GRAPH_DEGREE; k++) {
      if (node->refs[k]
          != (struct graph_node_s *)GC_REVEAL_POINTER(
              graph_table[graph_ref(i, k)]))
        ok = 0;
    }
    if (!ok)
      n_bad++;
  }
  return n_bad;
}

#ifdef GROW_MARK_RING
/*
 * Collect from a global mark stack of `stack_size` entries, refusing the
 * first request of memory from the OS, and check the shapes.
 */
static void
check_refused_growth(size_t stack_size)
{
  size_t list_bad, graph_bad;

  LOCK();
  alloc_mark_stack(stack_size);
  GC_after_marking();
  UNLOCK();
  n_no_room = 0;
  n_overflows = 0;
  n_ring_grown = 0;
  n_refused = 0;
  refuse_next_get_mem = TRUE;
  GC_gcollect();
  refuse_next_get_mem = FALSE;
  GC_disable();
  reuse_memory();
  list_bad = check_list();
  graph_bad = check_graph();
  GC_enable();
  printf("First request of memory refused, from a mark stack of %lu"
         " entries: %lu list objects and %lu graph nodes reclaimed, %lu"
         " requests refused, %lu local mark stacks dropped, %lu overflows"
         " dropping entries, %lu growths of the ring, mark stack of %lu"
         " entries\n",
         (unsigned long)stack_size, (unsigned long)list_bad,
         (unsigned long)graph_bad, (unsigned long)n_refused,
         (unsigned long)n_no_room, (unsigned long)n_overflows,
         (unsigned long)n_ring_grown, (unsigned long)GC_mark_stack_size);
  TEST_ASSERT(0 == list_bad);
  TEST_ASSERT(0 == graph_bad);
  /* A refused request makes the collector drop entries. */
  TEST_ASSERT(n_refused <= 1);
  TEST_ASSERT(0 == n_refused || n_no_room > 0 || n_overflows > 0);
}
#endif

#ifdef CHECK_RING
static void
check_ring(size_t stack_size)
{
  size_t list_bad;
  size_t i;
  unsigned k;

  /* A stale pointer to a node of the graph keeps no other node alive. */
  for (i = 0; i < graph_nodes; i++) {
    struct graph_node_s *node
        = (struct graph_node_s *)GC_REVEAL_POINTER(graph_table[i]);

    for (k = 0; k < GRAPH_DEGREE; k++)
      GC_ptr_store_and_dirty(&node->refs[k], NULL);
  }
  graph = NULL;
  graph_table = NULL;
  graph_nodes = 0;
  /* Make the mark stack `stack_size` entries long, and recycle the old one. */
  LOCK();
  alloc_mark_stack(stack_size);
  GC_after_marking();
  UNLOCK();
  n_no_room = 0;
  GC_gcollect();
  GC_disable();
  reuse_memory();
  list_bad = check_list();
  GC_enable();
  printf("List only, from a mark stack of %lu entries: %lu list objects"
         " reclaimed, %lu local mark stacks dropped, last of %lu parallel"
         " mark phases put %lu entries on the global mark stack of %lu"
         " entries\n",
         (unsigned long)stack_size, (unsigned long)list_bad,
         (unsigned long)n_no_room, (unsigned long)n_phases,
         (unsigned long)n_ring_entries, (unsigned long)ring_size);
  TEST_ASSERT(0 == list_bad);
  if (stack_size >= 2 * LOCAL_MARK_STACK_SIZE && stack_size <= list_len / 2) {
    /*
     * The marking of the list puts nearly an entry per node on the global
     * mark stack, while the markers keep few of them waiting.  With one
     * marker, the collection has no parallel mark phase.
     */
    TEST_ASSERT(0 == n_no_room);
    if (n_phases > 0)
      TEST_ASSERT(n_ring_entries > ring_size);
  }
}
#endif

int
main(void)
{
  size_t stack_size;
  int i;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    return 0;
  }
  /* Mark in parallel, if available, although no thread is created. */
  GC_start_mark_threads();
  GC_set_on_collection_event(on_collection_event);
#ifdef INITIAL_MARK_STACK_SIZE
  stack_size = INITIAL_MARK_STACK_SIZE;
#else
  stack_size = GC_get_hblk_size();
#endif
  list_len = stack_size < MAX_LIST_LEN / LIST_LEN_PER_STACK
                 ? stack_size * LIST_LEN_PER_STACK
                 : MAX_LIST_LEN;
  graph_nodes = stack_size < MAX_GRAPH_NODES / GRAPH_NODES_PER_STACK
                    ? stack_size * GRAPH_NODES_PER_STACK
                    : MAX_GRAPH_NODES;
  printf("List of %lu nodes, graph of %lu nodes (initial mark stack of %lu"
         " entries)\n",
         (unsigned long)list_len, (unsigned long)graph_nodes,
         (unsigned long)stack_size);

  /* No collection marks from a part of the shapes only. */
  GC_disable();
  make_list();
  make_graph();
  GC_enable();
  for (i = 1; i <= N_COLLECTIONS; i++) {
    size_t list_bad, graph_bad;

    n_no_room = 0;
#ifdef GROW_MARK_RING
    n_ring_grown = 0;
#endif
    GC_gcollect();
    /* No collection runs until the objects are checked. */
    GC_disable();
    reuse_memory();
    list_bad = check_list();
    graph_bad = check_graph();
    GC_enable();
    printf("Collection %d: %lu list objects and %lu graph nodes reclaimed,"
           " %lu local mark stacks dropped, mark stack of %lu entries\n",
           i, (unsigned long)list_bad, (unsigned long)graph_bad,
           (unsigned long)n_no_room, (unsigned long)GC_mark_stack_size);
    TEST_ASSERT(0 == list_bad);
    TEST_ASSERT(0 == graph_bad);
#ifdef CHECK_RING
    if (n_phases > 0) {
      printf("Last of %lu parallel mark phases: %lu entries put on the global"
             " mark stack of %lu entries\n",
             (unsigned long)n_phases, (unsigned long)n_ring_entries,
             (unsigned long)ring_size);
    }
#endif
#ifdef GROW_MARK_RING
    printf("Collection %d: %lu growths of the ring\n", i,
           (unsigned long)n_ring_grown);
    /* The ring grows instead. */
    TEST_ASSERT(0 == n_no_room);
#endif
  }
#ifdef GROW_MARK_RING
  check_refused_growth(stack_size);
#endif
#ifdef CHECK_RING
  /* A ring of at least two local mark stacks. */
  check_ring(stack_size < 2 * LOCAL_MARK_STACK_SIZE
                 ? (size_t)2 * LOCAL_MARK_STACK_SIZE
                 : stack_size);
#endif
  printf("SUCCEEDED\n");
  return 0;
}
