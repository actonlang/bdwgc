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
 * Test the growth of the global mark stack in serial marking: when it
 * overflows, its entries are moved to a larger mark stack, and no entry
 * is dropped unless the memory of the larger mark stack cannot be
 * obtained.  The live data overflows a small mark stack in several ways:
 * a list whose nodes have a mark procedure that pushes a payload ahead
 * of the next node (thus marking the list leaves an entry per node on
 * the mark stack), the same with a plain list, a mark procedure pushing
 * three mark stacks' worth of entries in one call, the procedure set by
 * `GC_set_on_mark_stack_empty()` and the one set by
 * `GC_set_push_other_roots()` doing the same, a list reachable only from
 * a finalizable object (marked by `GC_finalize()`), and an array of three
 * mark stacks' worth of payloads with a finalizer which ignores
 * self-pointers (thus the mark procedure of the finalization pushes all
 * of the payloads, before the marking from them).  Each payload
 * refers to a pointer-free leaf.  After each collection the test
 * allocates objects of the size of a leaf, so that the memory of the
 * leaves, if they were reclaimed, is reused, and then checks every leaf.
 * A disappearing link to the first node of the list of the finalizable
 * object tells whether the collector marked the list from that object;
 * if a stale copy of a pointer kept the list reachable from the roots,
 * or the object itself, then the growth part is repeated with a new list.
 * At the end of the marking from the roots the test counts the nodes left
 * unmarked, which the collector marks from the object (a stale copy of a
 * pointer to a node keeps the rest of the list reachable).  The finalizers
 * clear the references of their objects, the one of the list also the
 * links between its nodes, so that a stale copy of a pointer to any of
 * these keeps nothing else reachable in the later collections.
 *
 * The collections are done with a stop function, which disables the
 * parallel marking.  The mark procedure of the list nodes counts its
 * calls per node: a node is marked from once per collection, unless the
 * marking is completed by marking from all the marked objects again
 * (after entries were dropped), which marks from each marked node once
 * more per scan of the heap.  This test includes the collector source to
 * reset the mark stack to its initial size before each part, to refuse
 * the requests of memory from the OS during a collection on demand, and
 * to read the numbers of the moves and the drops of the global mark stack
 * entries of a collection.  The parts are: no refusal (no entry is
 * dropped), the array with the finalizer ignoring self-pointers, the
 * first request refused (the entries are dropped once, and the heap is
 * scanned once), every request refused (the mark stack does not grow),
 * the mark state invalidated while entries wait in the larger mark stack
 * (as a fault while pushing the roots does), and incremental collections
 * with a time limit (if supported).
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

/* Which requests of memory from the OS to refuse during a collection. */
enum refusal { REFUSE_NONE, REFUSE_FIRST, REFUSE_ALL };

static enum refusal refusal_mode = REFUSE_NONE;

/* Whether a collection of this test is in progress. */
static GC_bool in_collection;

/* The number of the requests made and refused during the collections. */
static unsigned n_requests, n_refused;

static ptr_t
get_test_memory(size_t bytes)
{
  if (in_collection && refusal_mode != REFUSE_NONE) {
    n_requests++;
    if (REFUSE_ALL == refusal_mode || 1 == n_requests) {
      n_refused++;
      return NULL;
    }
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

#include <string.h>

#ifndef GC_NO_FINALIZATION

#  define TEST_ASSERT(e)                                                      \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
                #e);                                                          \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define CHECK_OUT_OF_MEMORY(p)            \
    do {                                    \
      if (NULL == (p)) {                    \
        fprintf(stderr, "Out of memory\n"); \
        exit(69);                           \
      }                                     \
    } while (0)

/*
 * The largest initial mark stack this test runs with.  The lengths of
 * the lists are multiples of its size.
 */
#  define MAX_STACK_SIZE ((size_t)1 << 16)

/*
 * The maximum length of the list reachable from the finalizable object,
 * and of the array with a finalizer.
 */
#  define MAX_FIN_LIST_LEN ((size_t)1 << 20)

#  define LEAF_MAGIC ((GC_word)0x5a5a4321UL)

/* The first word of a node or a fan (odd, unlike a free-list link). */
#  define OBJ_TAG ((GC_word)0x75)

/*
 * The number of the rounds of the growth part, i.e. of the lists of the
 * finalizable objects made, to find a list not reachable from the roots
 * (as a stale copy of a pointer to it may be found by the collector).
 */
#  define MAX_ROUNDS 4

/* The number of the stack bytes cleared after the data is made. */
#  define CLEAR_STACK_BYTES 32768

/* The number of the words of each frame of `clear_stack()`. */
#  define CLEAR_FRAME_WORDS 512

/*
 * The number of the collections after which the finalizer should have
 * run (more than one in case a stale copy of a pointer to the object
 * is found by the collector).
 */
#  define MAX_FIN_COLLECTIONS 10

/* The initial size of the mark stack, in entries. */
static size_t stack_size;

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

/*
 * A node of the list whose nodes have a mark procedure.  The procedure
 * pushes the payload, then the next node, thus the payload stays on the
 * mark stack while the rest of the list is marked.
 */
struct node_s {
  GC_word tag;
  void **payload;
  struct node_s *next;
  GC_word id;
};

/*
 * An object whose mark procedure pushes the `n` payloads of `items`, then
 * `items` itself (thus a conservative scan reaches the same objects).
 */
struct fan_s {
  GC_word tag;
  GC_word n;
  void ***items;
};

/*
 * The size of the nodes and of the fan, three granules.  The mark
 * procedure of a small object of one, two or four granules might not
 * be called when the marked objects of its block are pushed (these are
 * scanned conservatively then).
 */
#  define PROC_OBJ_BYTES (3 * GC_GRANULE_BYTES - EXTRA_BYTES)

static int node_kind, fan_kind;

/* Whether to count the calls of the mark procedure of the nodes. */
static volatile GC_bool count_marks;

/* The calls of the mark procedure per node (not in the heap). */
static unsigned char *mark_counts;

static struct node_s *proc_list;
static size_t proc_list_len;

/* A list whose nodes are pairs of a payload and the next node. */
static void **plain_list;
static size_t plain_list_len;

static struct fan_s *fan;

/*
 * The payloads pushed by `on_mark_stack_empty()` and by `push_roots()`,
 * not allocated by the collector, thus marked only by these.
 */
static void ***empty_items, ***root_items;
static size_t n_items;

/* Whether `push_roots()` should invalidate the mark state. */
static GC_bool invalidate_in_push;

static GC_push_other_roots_proc old_push_other_roots;

/*
 * The payloads of `on_mark_stack_empty()`, for the incremental part.
 * This is volatile: the compiler may make a static only used by `main()`
 * a local variable of it (in C++, where `main()` cannot be called
 * recursively), which is dead after the payloads are stored.
 */
static void ***volatile empty_keep;

/* The moves of the global mark stack entries, and the drops of them. */
static word n_moves, n_moved_entries, n_drops;

/* The size of the mark stack at the end of the marking of a collection. */
static size_t size_at_mark_end;

/*
 * Whether to count the nodes of the list of the finalizable object (see
 * below) left unmarked at the end of the marking from the roots, and
 * their number in the last such collection, if the object was not marked
 * then.  The collector marks these nodes from the object.
 */
static GC_bool count_unmarked_nodes;
static size_t n_unmarked_nodes;

/* The warnings about a failed growth of the mark stack. */
static unsigned n_grow_warnings;

static struct GC_ms_entry *GC_CALLBACK
node_mark_proc(GC_word *addr, struct GC_ms_entry *mark_stack_top,
               struct GC_ms_entry *mark_stack_limit, GC_word env)
{
  struct node_s *node = (struct node_s *)addr;

  (void)env;
  /* A free-list object starts with a link (or `NULL`), not the tag. */
  if (node->tag != OBJ_TAG)
    return mark_stack_top;
  if (count_marks && node->id < proc_list_len && mark_counts[node->id] < 255)
    mark_counts[node->id]++;
  mark_stack_top = GC_MARK_AND_PUSH(node->payload, mark_stack_top,
                                    mark_stack_limit, (void **)&node->payload);
  return GC_MARK_AND_PUSH(node->next, mark_stack_top, mark_stack_limit,
                          (void **)&node->next);
}

static struct GC_ms_entry *GC_CALLBACK
fan_mark_proc(GC_word *addr, struct GC_ms_entry *mark_stack_top,
              struct GC_ms_entry *mark_stack_limit, GC_word env)
{
  const struct fan_s *f = (const struct fan_s *)addr;
  GC_word i;

  (void)env;
  if (f->tag != OBJ_TAG)
    return mark_stack_top;
  /* Each push marks an object, thus this ends after an overflow too. */
  for (i = 0; i < f->n; i++) {
    mark_stack_top = GC_MARK_AND_PUSH(f->items[i], mark_stack_top,
                                      mark_stack_limit, (void **)&f->items[i]);
  }
  return GC_MARK_AND_PUSH(f->items, mark_stack_top, mark_stack_limit,
                          (void **)&f->items);
}

static struct GC_ms_entry *GC_CALLBACK
on_mark_stack_empty(struct GC_ms_entry *mark_stack_top,
                    struct GC_ms_entry *mark_stack_limit)
{
  size_t i;

  /* No entry is pushed for a marked object, thus this ends. */
  for (i = 0; i < n_items; i++) {
    mark_stack_top
        = GC_mark_and_push(empty_items[i], mark_stack_top, mark_stack_limit,
                           (void **)&empty_items[i]);
  }
  return mark_stack_top;
}

static void GC_CALLBACK
push_roots(void)
{
  if (old_push_other_roots != 0)
    old_push_other_roots();
  /* Push an entry per payload, as the eager root scan does. */
  GC_push_all_eager(root_items, root_items + n_items);
  if (invalidate_in_push && GC_next_mark_stack != NULL) {
    /*
     * A fault while pushing the roots invalidates the mark state, as
     * this does, and the marking is completed by marking from all the
     * marked objects.
     */
    invalidate_in_push = FALSE;
    GC_invalidate_mark_state();
  }
}

static void count_unmarked(void);

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
  case GC_EVENT_START:
    in_collection = TRUE;
    break;
  case GC_EVENT_MARK_END:
    /* `GC_after_marking()` is not called yet. */
    n_moves += GC_mark_stack_events.n_moves;
    n_moved_entries += GC_mark_stack_events.moved_entries;
    n_drops += GC_mark_stack_events.n_overflows;
    size_at_mark_end = GC_mark_stack_size;
    if (count_unmarked_nodes)
      count_unmarked();
    break;
  case GC_EVENT_END:
    in_collection = FALSE;
    break;
  default:
    break;
  }
}

static void GC_CALLBACK
warn_proc(const char *msg, GC_uintptr_t arg)
{
  if (strstr(msg, "Failed to grow mark stack") != NULL) {
    n_grow_warnings++;
  } else if (strstr(msg, "while GC was disabled") == NULL) {
    GC_default_warn_proc(msg, arg);
  }
}

static void
make_data(void)
{
  size_t i;

  for (i = proc_list_len; i > 0; i--) {
    struct node_s *node
        = (struct node_s *)GC_generic_malloc(PROC_OBJ_BYTES, node_kind);

    CHECK_OUT_OF_MEMORY(node);
    node->tag = OBJ_TAG;
    node->id = (GC_word)(i - 1);
    GC_ptr_store_and_dirty(&node->payload, make_payload(node->id));
    GC_ptr_store_and_dirty(&node->next, proc_list);
    proc_list = node;
  }
  for (i = plain_list_len; i > 0; i--) {
    void **node = (void **)GC_MALLOC(2 * sizeof(void *));

    CHECK_OUT_OF_MEMORY(node);
    GC_ptr_store_and_dirty(&node[0], make_payload((GC_word)(i - 1)));
    GC_ptr_store_and_dirty(&node[1], plain_list);
    plain_list = node;
  }
  fan = (struct fan_s *)GC_generic_malloc(PROC_OBJ_BYTES, fan_kind);
  CHECK_OUT_OF_MEMORY(fan);
  fan->tag = OBJ_TAG;
  fan->n = (GC_word)n_items;
  GC_end_stubborn_change(fan);
  GC_ptr_store_and_dirty(&fan->items,
                         GC_MALLOC_IGNORE_OFF_PAGE(n_items * sizeof(void **)));
  CHECK_OUT_OF_MEMORY(fan->items);
  empty_items = (void ***)malloc(n_items * sizeof(void **));
  root_items = (void ***)malloc(n_items * sizeof(void **));
  CHECK_OUT_OF_MEMORY(empty_items);
  CHECK_OUT_OF_MEMORY(root_items);
  for (i = 0; i < n_items; i++) {
    GC_ptr_store_and_dirty(&fan->items[i], make_payload((GC_word)i));
    empty_items[i] = make_payload((GC_word)i);
    root_items[i] = make_payload((GC_word)i);
  }
}

/* Check every leaf of the live data. */
static void
check_data(void)
{
  const struct node_s *node = proc_list;
  void **plain_node = plain_list;
  size_t i, n_bad = 0;

  for (i = 0; i < proc_list_len; i++) {
    TEST_ASSERT(node != NULL && node->id == (GC_word)i);
    if (!payload_ok(node->payload, (GC_word)i))
      n_bad++;
    node = node->next;
  }
  TEST_ASSERT(NULL == node);
  for (i = 0; i < plain_list_len; i++) {
    TEST_ASSERT(plain_node != NULL);
    if (!payload_ok((void **)plain_node[0], (GC_word)i))
      n_bad++;
    plain_node = (void **)plain_node[1];
  }
  TEST_ASSERT(NULL == plain_node);
  for (i = 0; i < n_items; i++) {
    if (!payload_ok(fan->items[i], (GC_word)i)
        || !payload_ok(empty_items[i], (GC_word)i)
        || !payload_ok(root_items[i], (GC_word)i))
      n_bad++;
  }
  if (n_bad != 0) {
    fprintf(stderr, "%lu payloads reclaimed\n", (unsigned long)n_bad);
    exit(1);
  }
}

/* The object with a finalizer, which refers to a list. */
struct holder_s {
  void **list;
  GC_word len;
};

static size_t fin_list_len;

/* The number of the payloads of the array with a finalizer, or 0. */
static size_t fin_array_len;

/*
 * A disappearing link to the first node of the list, hidden so that it
 * does not keep the node reachable.  The collector clears it if it finds
 * the node unreachable from the roots, before it marks from the objects
 * which become finalizable.
 */
static GC_hidden_pointer head_link;

/*
 * The object with the finalizer, hidden so that it does not keep the
 * object reachable.  The object is not reclaimed before its finalizer
 * runs, thus the pointer can be revealed until then.
 */
static GC_hidden_pointer holder_link;

static unsigned n_finalized;
static size_t n_bad_fin_leaves;

static void GC_CALLBACK
check_fin_list(void *obj, void *cd)
{
  struct holder_s *h = (struct holder_s *)obj;
  void **node = h->list;
  GC_word i;

  for (i = 0; i < h->len; i++) {
    void **next = (void **)node[1];

    if (!payload_ok((void **)node[0], i))
      n_bad_fin_leaves++;
    /* The node no longer keeps the rest of the list. */
    node[1] = NULL;
    node = next;
  }
  TEST_ASSERT(NULL == node);
  /* The object no longer keeps the list. */
  h->list = NULL;
  n_finalized++;
  (void)cd;
}

/*
 * Allocate a list and an object with a finalizer referring to it.
 * Nothing else refers to the object.  Called through a pointer (see
 * below), so that no pointer to the object or to the list remains in
 * the frame of the caller.
 */
static void
make_holder(void)
{
  void **list = NULL;
  struct holder_s *h;
  size_t i;

  for (i = fin_list_len; i > 0; i--) {
    void **node = (void **)GC_MALLOC(2 * sizeof(void *));

    CHECK_OUT_OF_MEMORY(node);
    GC_ptr_store_and_dirty(&node[0], make_payload((GC_word)(i - 1)));
    GC_ptr_store_and_dirty(&node[1], list);
    list = node;
  }
  h = GC_NEW(struct holder_s);
  CHECK_OUT_OF_MEMORY(h);
  h->len = (GC_word)fin_list_len;
  GC_ptr_store_and_dirty(&h->list, list);
  head_link = GC_HIDE_POINTER(list);
  /*
   * The macro registers the link for the base address of the node, which
   * is that of the debug header of the node if `GC_DEBUG` is defined.
   */
  TEST_ASSERT(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&head_link, list)
              == GC_SUCCESS);
  holder_link = GC_HIDE_POINTER(h);
  GC_REGISTER_FINALIZER(h, check_fin_list, NULL, NULL, NULL);
  /*
   * The collector may find a copy of the start address of the latest heap
   * section on the stack.  If the heap grew while the list was made, then
   * that is the address of a block of the list, thus grow the heap once
   * more.
   */
  (void)GC_expand_hp(MINHINCR * HBLKSIZE);
  n_finalized = 0;
  n_bad_fin_leaves = 0;
}

static void GC_CALLBACK
check_fin_array(void *obj, void *cd)
{
  void ***arr = (void ***)obj;
  size_t i;

  for (i = 0; i < fin_array_len; i++) {
    if (!payload_ok(arr[i], (GC_word)i))
      n_bad_fin_leaves++;
    /* The array no longer keeps the payload. */
    arr[i] = NULL;
  }
  n_finalized++;
  (void)cd;
}

/*
 * Allocate an array of `fin_array_len` payloads with a finalizer which
 * ignores self-pointers.  Nothing else refers to the array or to the
 * payloads.  Called through a pointer, as `make_holder()`.
 */
static void
make_fin_array(void)
{
  void ***arr
      = (void ***)GC_MALLOC_IGNORE_OFF_PAGE(fin_array_len * sizeof(void **));
  size_t i;

  CHECK_OUT_OF_MEMORY(arr);
  for (i = 0; i < fin_array_len; i++)
    GC_ptr_store_and_dirty(&arr[i], make_payload((GC_word)i));
  holder_link = GC_HIDE_POINTER(arr);
  GC_REGISTER_FINALIZER_IGNORE_SELF(arr, check_fin_array, NULL, NULL, NULL);
  /* See `make_holder()`. */
  (void)GC_expand_hp(MINHINCR * HBLKSIZE);
  n_finalized = 0;
  n_bad_fin_leaves = 0;
}

static GC_word clear_stack(size_t bytes);

/* These are called through pointers to prevent inlining. */
static void (*volatile make_holder_p)(void) = make_holder;
static void (*volatile make_fin_array_p)(void) = make_fin_array;
static GC_word (*volatile clear_stack_p)(size_t) = clear_stack;

/*
 * Overwrite `bytes` bytes of the stack below the frame of the caller,
 * where the data was made, so that the collector does not find stale
 * pointers there.  This calls itself, each frame overwriting
 * `CLEAR_FRAME_WORDS` words, to keep the frames small.  The returned
 * value is not used.
 */
static GC_word
clear_stack(size_t bytes)
{
  volatile GC_word words[CLEAR_FRAME_WORDS];
  size_t i;

  for (i = 0; i < CLEAR_FRAME_WORDS; i++)
    words[i] = 0;
  /* The result is stored, thus this is not a tail call. */
  if (bytes > sizeof(words))
    words[0] = clear_stack_p(bytes - sizeof(words));
  return words[0];
}

/*
 * Set `n_unmarked_nodes` at the end of the marking from the roots, with
 * the allocator lock held, unless the finalizer of the object has run.
 */
static void
count_unmarked(void)
{
  const struct holder_s *h;
  void *const *node;
  size_t n = 0;

  if (n_finalized > 0)
    return;
  h = (const struct holder_s *)GC_REVEAL_POINTER(holder_link);
  if (!GC_is_marked(GC_base((void *)h))) {
    for (node = h->list; node != NULL; node = (void *const *)node[1]) {
      if (!GC_is_marked(GC_base((void *)node)))
        n++;
    }
  }
  n_unmarked_nodes = n;
}

/*
 * Unregister the finalizer of the object (with the list, or the array),
 * after a stale copy of a pointer kept the object reachable, and drop its
 * references.  Called through a pointer, so that the revealed pointer to
 * the object is in a frame which `clear_stack()` overwrites.
 */
static void
unregister_holder(void)
{
  void *obj = GC_REVEAL_POINTER(holder_link);
  size_t i;

  GC_REGISTER_FINALIZER(obj, 0, NULL, NULL, NULL);
  if (fin_array_len > 0) {
    for (i = 0; i < fin_array_len; i++)
      ((void ***)obj)[i] = NULL;
  } else {
    struct holder_s *h = (struct holder_s *)obj;
    void **node = h->list;

    TEST_ASSERT(GC_unregister_disappearing_link((void **)&head_link));
    h->list = NULL;
    /* Clear the links between the nodes, as the finalizer does. */
    while (node != NULL) {
      void **next = (void **)node[1];

      node[1] = NULL;
      node = next;
    }
  }
}

static void (*volatile unregister_holder_p)(void) = unregister_holder;

/* Allocate objects of the size and kind of a leaf, many times. */
static void
reuse_leaves(void)
{
  size_t i;
  size_t n = 2
             * (proc_list_len + plain_list_len + 3 * n_items + fin_list_len
                + fin_array_len);

  for (i = 0; i < n; i++) {
    GC_word *p = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));

    CHECK_OUT_OF_MEMORY(p);
    p[0] = ~(GC_word)0;
    p[1] = ~(GC_word)0;
  }
}

static int GC_CALLBACK
never_stop(void)
{
  return 0;
}

/*
 * Make the mark stack its initial size again, and recycle the larger
 * one now (not in the next collection, when requests may be refused).
 */
static void
reset_mark_stack(void)
{
  LOCK();
  GC_ASSERT(!GC_collection_in_progress());
  alloc_mark_stack(stack_size);
  GC_after_marking();
  UNLOCK();
}

/*
 * Do a collection with the parallel marking disabled, then check the
 * live data.  Return the largest count of the marks of a node.
 */
static unsigned
collect(enum refusal r)
{
  unsigned max_count = 0;
  size_t i;

  memset(mark_counts, 0, proc_list_len);
  refusal_mode = r;
  n_requests = 0;
  count_marks = TRUE;
  GC_enable();
  TEST_ASSERT(GC_try_to_collect(never_stop));
  GC_disable();
  count_marks = FALSE;
  refusal_mode = REFUSE_NONE;
  reuse_leaves();
  (void)GC_invoke_finalizers();
  check_data();
  for (i = 0; i < proc_list_len; i++) {
    if (mark_counts[i] > max_count)
      max_count = mark_counts[i];
    /* Every node is reachable, thus marked from at least once. */
    TEST_ASSERT(mark_counts[i] > 0);
  }
  return max_count;
}

static void
test_growth(void)
{
  unsigned max_count;
  int round, i;

  for (round = 1; round <= MAX_ROUNDS; round++) {
    reset_mark_stack();
    make_holder_p();
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    count_unmarked_nodes = TRUE;
    for (i = 1; i <= MAX_FIN_COLLECTIONS && 0 == n_finalized; i++) {
      size_t size_before = GC_mark_stack_size;

      n_moves = 0;
      n_moved_entries = 0;
      n_drops = 0;
      max_count = collect(REFUSE_NONE);
      printf("Collection %d of round %d: %lu entries moved %lu times,"
             " %lu drops, mark stack of %lu entries (%lu after the"
             " marking), node marked from at most %u times\n",
             i, round, (unsigned long)n_moved_entries, (unsigned long)n_moves,
             (unsigned long)n_drops, (unsigned long)GC_mark_stack_size,
             (unsigned long)size_at_mark_end, max_count);
      TEST_ASSERT(0 == n_drops);
      TEST_ASSERT(1 == max_count);
      TEST_ASSERT(0 == n_refused);
      if (1 == i) {
        TEST_ASSERT(n_moves > 0);
        TEST_ASSERT(size_at_mark_end > size_before);
      }
    }
    count_unmarked_nodes = FALSE;
    if (0 == n_finalized) {
      printf("Finalizer not run in %d collections of round %d\n",
             MAX_FIN_COLLECTIONS, round);
      unregister_holder_p();
      continue;
    }
    printf("Finalizer run after collection %d, %lu leaves reclaimed,"
           " list %s from the roots, %lu nodes unmarked then\n",
           i - 1, (unsigned long)n_bad_fin_leaves,
           0 == head_link ? "unreachable" : "reachable",
           (unsigned long)n_unmarked_nodes);
    TEST_ASSERT(0 == n_bad_fin_leaves);
    if (0 == head_link) {
      /*
       * The list was marked from the finalizable object, but for the nodes
       * which a stale copy of a pointer to a node kept reachable.  If more
       * nodes were left than the mark stack has entries after the marking,
       * then that marking (in `GC_finalize()`) grew the mark stack.
       */
      if (n_unmarked_nodes > size_at_mark_end)
        TEST_ASSERT(GC_mark_stack_size > size_at_mark_end);
      break;
    }
    TEST_ASSERT(GC_unregister_disappearing_link((void **)&head_link));
  }
  if (round > MAX_ROUNDS) {
    fprintf(stderr, "Every object or list was reachable from the roots\n");
    exit(1);
  }
  /* No more growth needed. */
  n_moves = 0;
  TEST_ASSERT(1 == collect(REFUSE_NONE));
  TEST_ASSERT(0 == n_moves);
}

/*
 * Mark from an array with a finalizer which ignores self-pointers.  Its
 * finalization mark procedure pushes an entry per payload onto the global
 * mark stack, three times as many as the mark stack has, thus it
 * overflows the mark stack itself before `GC_mark_fo()` marks from the
 * pushed entries.
 */
static void
test_fin_array(void)
{
  int round, i;

  reset_mark_stack();
  /* Grow the mark stack to the size the marking of the data needs. */
  (void)collect(REFUSE_NONE);
  fin_array_len = 3 * GC_mark_stack_size < MAX_FIN_LIST_LEN
                      ? 3 * GC_mark_stack_size
                      : MAX_FIN_LIST_LEN;
  for (round = 1; round <= MAX_ROUNDS; round++) {
    make_fin_array_p();
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    for (i = 1; i <= MAX_FIN_COLLECTIONS && 0 == n_finalized; i++) {
      n_drops = 0;
      /* A node marked from twice would tell of a scan after a drop. */
      TEST_ASSERT(1 == collect(REFUSE_NONE));
      printf("Collection %d of round %d with the array of %lu payloads: %lu"
             " drops, mark stack of %lu entries (%lu after the marking)\n",
             i, round, (unsigned long)fin_array_len, (unsigned long)n_drops,
             (unsigned long)GC_mark_stack_size,
             (unsigned long)size_at_mark_end);
      TEST_ASSERT(0 == n_drops);
      if (n_finalized > 0 && fin_array_len > size_at_mark_end) {
        /* `GC_finalize()` marked from the array and grew the mark stack. */
        TEST_ASSERT(GC_mark_stack_size > size_at_mark_end);
      }
    }
    if (n_finalized > 0)
      break;
    /* A stale copy of a pointer kept the array reachable. */
    unregister_holder_p();
  }
  TEST_ASSERT(1 == n_finalized);
  printf("Finalizer of the array run after collection %d, %lu leaves"
         " reclaimed\n",
         i - 1, (unsigned long)n_bad_fin_leaves);
  TEST_ASSERT(0 == n_bad_fin_leaves);
  fin_array_len = 0;
}

static void
test_refuse_first(void)
{
  unsigned max_count;

  reset_mark_stack();
  n_moves = 0;
  n_drops = 0;
  n_refused = 0;
  max_count = collect(REFUSE_FIRST);
  printf("First request refused: %lu moves, %lu drops, %u requests,"
         " node marked from at most %u times\n",
         (unsigned long)n_moves, (unsigned long)n_drops, n_requests,
         max_count);
  TEST_ASSERT(1 == n_refused);
  TEST_ASSERT(n_drops > 0);
  /* The heap is scanned for the marked objects once. */
  TEST_ASSERT(2 == max_count);
  TEST_ASSERT(GC_mark_stack_size > stack_size);
}

static void
test_refuse_all(void)
{
  unsigned max_count;
  int round, i;

  for (round = 1; round <= MAX_ROUNDS; round++) {
    reset_mark_stack();
    make_holder_p();
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    for (i = 1; i <= MAX_FIN_COLLECTIONS; i++) {
      n_moves = 0;
      n_drops = 0;
      n_refused = 0;
      n_grow_warnings = 0;
      max_count = collect(REFUSE_ALL);
      printf("Every request refused: %lu drops, %u requests,"
             " %u warnings, node marked from at most %u times\n",
             (unsigned long)n_drops, n_requests, n_grow_warnings, max_count);
      TEST_ASSERT(n_refused > 0 && n_refused == n_requests);
      TEST_ASSERT(n_drops > 0 && 0 == n_moves);
      TEST_ASSERT(n_grow_warnings > 0);
      TEST_ASSERT(max_count >= 2);
      TEST_ASSERT(GC_mark_stack_size == stack_size);
      if (n_finalized > 0) {
        printf("Finalizer run after collection %d, %lu leaves reclaimed\n", i,
               (unsigned long)n_bad_fin_leaves);
        TEST_ASSERT(0 == n_bad_fin_leaves);
        break;
      }
    }
    if (n_finalized > 0)
      break;
    printf("Finalizer not run in %d collections of round %d\n",
           MAX_FIN_COLLECTIONS, round);
    unregister_holder_p();
  }
  TEST_ASSERT(1 == n_finalized);
}

static void
test_invalidate(void)
{
  unsigned max_count;

  reset_mark_stack();
  invalidate_in_push = TRUE;
  n_moves = 0;
  n_drops = 0;
  max_count = collect(REFUSE_NONE);
  printf("Mark state invalidated after %lu moves: %lu drops,"
         " node marked from at most %u times\n",
         (unsigned long)n_moves, (unsigned long)n_drops, max_count);
  TEST_ASSERT(!invalidate_in_push);
  TEST_ASSERT(0 == n_drops);
  TEST_ASSERT(max_count <= 2);
}

static void
test_incremental(void)
{
  int i;

  GC_enable_incremental();
  if (!GC_is_incremental_mode()) {
    printf("Incremental mode not supported\n");
    return;
  }
  /*
   * A full collection may mark in parallel, which does not call
   * `on_mark_stack_empty()`, thus keep its payloads reachable.
   */
  empty_keep = (void ***)GC_MALLOC_IGNORE_OFF_PAGE(n_items * sizeof(void **));
  CHECK_OUT_OF_MEMORY(empty_keep);
  for (i = 0; i < (int)n_items; i++)
    GC_ptr_store_and_dirty(&empty_keep[i], empty_items[i]);
  reset_mark_stack();
  GC_set_time_limit(1);
  GC_enable();
  for (i = 0; i < 20; i++) {
    reuse_leaves();
    check_data();
  }
  GC_gcollect();
  GC_disable();
  reuse_leaves();
  check_data();
  printf("Incremental collections: %lu collections, mark stack of %lu"
         " entries\n",
         (unsigned long)GC_get_gc_no(), (unsigned long)GC_mark_stack_size);
}

int
main(void)
{
  GC_set_finalize_on_demand(1);
  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    return 0;
  }
  GC_set_warn_proc(warn_proc);
  stack_size = GC_mark_stack_size;
  if (stack_size > MAX_STACK_SIZE) {
    printf("Initial mark stack of %lu entries is too big for the test\n",
           (unsigned long)stack_size);
    return 0;
  }
  proc_list_len = 8 * stack_size;
  plain_list_len = 8 * stack_size;
  n_items = 3 * stack_size;
  fin_list_len = 64 * stack_size < MAX_FIN_LIST_LEN ? 64 * stack_size
                                                    : MAX_FIN_LIST_LEN;
  mark_counts = (unsigned char *)malloc(proc_list_len);
  CHECK_OUT_OF_MEMORY(mark_counts);
  node_kind = (int)GC_new_kind(
      GC_new_free_list(), GC_MAKE_PROC(GC_new_proc(node_mark_proc), 0), 0, 1);
  fan_kind = (int)GC_new_kind(
      GC_new_free_list(), GC_MAKE_PROC(GC_new_proc(fan_mark_proc), 0), 0, 1);
  GC_set_on_mark_stack_empty(on_mark_stack_empty);
  old_push_other_roots = GC_get_push_other_roots();
  GC_set_push_other_roots(push_roots);
  GC_set_on_collection_event(on_collection_event);
  printf("Initial mark stack of %lu entries\n", (unsigned long)stack_size);

  /* No collection runs, except the explicit ones. */
  GC_disable();
  make_data();
  test_growth();
  test_fin_array();
  test_refuse_first();
  test_refuse_all();
  test_invalidate();
  test_incremental();
  GC_enable();
  printf("SUCCEEDED\n");
  return 0;
}

#else

int
main(void)
{
  printf("test skipped\n");
  return 0;
}

#endif
