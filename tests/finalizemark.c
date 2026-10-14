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
 * Test that the objects reachable only from an unreachable finalizable
 * object survive the collection which finds it unreachable, even if the
 * mark stack overflows while the collector marks from that object.  The
 * object refers to a list whose nodes refer to a payload ahead of the
 * next node, thus marking the list leaves an entry on the mark stack per
 * node.  Each payload refers to a pointer-free leaf.  An overflow drops
 * the entries of some payloads (the payloads are marked already), thus
 * their leaves are marked only if the marking is completed.  After the
 * collection, the test allocates objects of the size of a leaf, so that
 * the memory of the leaves, if they were reclaimed, is reused, and then
 * the finalizer checks that every leaf is in an allocated block and
 * unchanged.  A disappearing link to the first node tells whether the
 * collector found the list unreachable from the roots, thus marked it
 * from the finalizable object.  If a stale copy of a pointer kept the
 * list or the object reachable, then the test is repeated with a new
 * list.  The list is several times longer than the initial mark stack (of
 * `INITIAL_MARK_STACK_SIZE` entries if the macro is defined, otherwise of
 * `HBLKSIZE` entries), thus the mark stack overflows even after it has
 * doubled twice.  This is tested for a finalizer registered by
 * `GC_register_finalizer()` and for one registered by
 * `GC_register_finalizer_no_order()` with the Java-style finalization
 * on, since the collector marks from the objects of these in different
 * steps.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"
#include "gc/gc_mark.h"

#ifndef GC_NO_FINALIZATION

#  define TEST_ASSERT(e)                                                    \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }

#  define CHECK_OUT_OF_MEMORY(p)            \
    do {                                    \
      if (NULL == (p)) {                    \
        fprintf(stderr, "Out of memory\n"); \
        exit(69);                           \
      }                                     \
    } while (0)

/* The length of the list, in initial mark stack sizes. */
#  define LIST_LEN_PER_STACK 8

/*
 * The maximum length of the list.  An initial mark stack with more
 * entries than this does not overflow in this test.
 */
#  define MAX_LIST_LEN ((size_t)1 << 21)

/*
 * The number of the collections after which the finalizer should have
 * run (more than one in case a stale copy of a pointer to the object
 * is found by the collector).
 */
#  define MAX_COLLECTIONS 10

#  define LEAF_MAGIC ((GC_word)0x5a5a4321UL)

/* The number of the stack bytes cleared after the list is made. */
#  define CLEAR_STACK_BYTES 32768

/* The number of the words of each frame of `clear_stack()`. */
#  define CLEAR_FRAME_WORDS 512

/*
 * The number of the rounds of each test, i.e. of the lists made, to find
 * a list not reachable from the roots (as a stale copy of a pointer to it
 * may be found by the collector).
 */
#  define MAX_ROUNDS 8

struct holder_s {
  void **list;
  GC_word len;
};

static size_t list_len;

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

/*
 * The number of the runs of the finalizer, and of the leaves it found
 * reclaimed.  The finalizer is run by `GC_invoke_finalizers()` only.
 */
static unsigned n_finalized;
static size_t n_bad_leaves;

static void GC_CALLBACK
check_list(void *obj, void *cd)
{
  struct holder_s *h = (struct holder_s *)obj;
  void **node = h->list;
  GC_word i;

  for (i = 0; i < h->len; i++) {
    const void *const *payload = (const void *const *)node[0];
    const GC_word *leaf = (const GC_word *)payload[0];

    if (NULL == GC_base((void *)leaf) || leaf[0] != (LEAF_MAGIC ^ i)
        || leaf[1] != i)
      n_bad_leaves++;
    node = (void **)node[1];
  }
  TEST_ASSERT(NULL == node);
  /* The object no longer keeps the list. */
  h->list = NULL;
  n_finalized++;
  (void)cd;
}

/*
 * Allocate the list and an object referring to it, and register the
 * finalizer of the object.  Nothing else refers to the object.  Called
 * through a pointer (see below), so that no pointer to the object or to
 * the list remains in the frame of the caller.
 */
static void
make_holder(int no_order)
{
  void **list = NULL;
  struct holder_s *h;
  size_t i;

  /* No collection marks from the list before the test one. */
  GC_disable();
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
  h = GC_NEW(struct holder_s);
  CHECK_OUT_OF_MEMORY(h);
  TEST_ASSERT(list != NULL);
  h->len = (GC_word)list_len;
  GC_ptr_store_and_dirty(&h->list, list);
  head_link = GC_HIDE_POINTER(list);
  /*
   * The macro registers the link for the base address of the node, which
   * is that of the debug header of the node if `GC_DEBUG` is defined.
   */
  TEST_ASSERT(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&head_link, list)
              == GC_SUCCESS);
  holder_link = GC_HIDE_POINTER(h);
  if (no_order) {
    GC_REGISTER_FINALIZER_NO_ORDER(h, check_list, NULL, NULL, NULL);
  } else {
    GC_REGISTER_FINALIZER(h, check_list, NULL, NULL, NULL);
  }
  GC_enable();
}

/* Called through a pointer to prevent inlining. */
static void (*volatile make_holder_p)(int) = make_holder;

/*
 * The words of the stack between the frame of the caller and that of
 * `make_holder()`, as a frame which does not hold pointers.
 */
#  define PAD_WORDS 64

/*
 * Call `make_holder()` below a frame of `PAD_WORDS` words.  A frame of
 * another callee of the caller, e.g. of `GC_gcollect()`, may have a slot
 * which it does not write, which keeps the value an earlier frame at its
 * place left there.  The frames of `make_holder()` and of its callees are
 * below such slots, where `clear_stack()` overwrites them.
 */
static void
make_holder_below(int no_order)
{
  volatile GC_word pad[PAD_WORDS];

  pad[0] = 0;
  make_holder_p(no_order);
  pad[PAD_WORDS - 1] = pad[0];
}

static GC_word clear_stack(size_t bytes);

/* These are called through pointers to prevent inlining. */
static void (*volatile make_holder_below_p)(int) = make_holder_below;
static GC_word (*volatile clear_stack_p)(size_t) = clear_stack;

/*
 * Overwrite `bytes` bytes of the stack below the frame of the caller,
 * where `make_holder()` and the allocation calls ran, so that the
 * collector does not find stale pointers to the object or to the list
 * there.  This calls itself, each frame overwriting `CLEAR_FRAME_WORDS`
 * words, to keep the frames small.  The returned value is not used.
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
 * Unregister the finalizer of the object and the link to the first node,
 * after a stale copy of a pointer kept the object reachable.  Called
 * through a pointer, so that the revealed pointer to the object is in a
 * frame which `clear_stack()` overwrites.
 */
static void
unregister_holder(void)
{
  struct holder_s *h = (struct holder_s *)GC_REVEAL_POINTER(holder_link);

  GC_REGISTER_FINALIZER(h, 0, NULL, NULL, NULL);
  TEST_ASSERT(GC_unregister_disappearing_link((void **)&head_link));
  /* The object no longer keeps the list. */
  h->list = NULL;
}

static void (*volatile unregister_holder_p)(void) = unregister_holder;

/*
 * Allocate and fill objects of the size and kind of a leaf, twice as many
 * as the leaves, so that the blocks of the leaves are swept.
 */
static void
reuse_leaves(void)
{
  size_t i;

  for (i = 0; i < 2 * list_len; i++) {
    GC_word *p = (GC_word *)GC_MALLOC_ATOMIC(2 * sizeof(GC_word));

    CHECK_OUT_OF_MEMORY(p);
    p[0] = ~(GC_word)0;
    p[1] = ~(GC_word)0;
  }
}

static void
test_finalizer(int no_order)
{
  int round;
  int i;

  for (round = 1; round <= MAX_ROUNDS; round++) {
    n_finalized = 0;
    n_bad_leaves = 0;
    make_holder_below_p(no_order);
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    for (i = 1; i <= MAX_COLLECTIONS && 0 == n_finalized; i++) {
      GC_gcollect();
      /* No collection runs until the finalizer has checked the leaves. */
      GC_disable();
      reuse_leaves();
      (void)GC_invoke_finalizers();
      GC_enable();
    }
    if (0 == n_finalized) {
      printf("Finalizer%s not run in %d collections of round %d\n",
             no_order ? " (no order)" : "", MAX_COLLECTIONS, round);
      unregister_holder_p();
      continue;
    }
    printf("Finalizer%s run after collection %d of round %d,"
           " %lu leaves reclaimed, list %s from the roots\n",
           no_order ? " (no order)" : "", i - 1, round,
           (unsigned long)n_bad_leaves,
           0 == head_link ? "unreachable" : "reachable");
    TEST_ASSERT(1 == n_finalized);
    TEST_ASSERT(0 == n_bad_leaves);
    if (0 == head_link) {
      /* The collector marked the list from the finalizable object. */
      return;
    }
    TEST_ASSERT(GC_unregister_disappearing_link((void **)&head_link));
  }
  fprintf(stderr, "Every object or list was reachable from the roots\n");
  exit(1);
}

int
main(void)
{
  size_t stack_size;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    return 0;
  }
  GC_set_finalize_on_demand(1);
  /*
   * Mark in parallel, if available, although no thread is created, as in
   * a multi-threaded client.  The marking from the finalizable objects is
   * done by one thread anyway.
   */
  GC_start_mark_threads();
#  ifdef INITIAL_MARK_STACK_SIZE
  stack_size = INITIAL_MARK_STACK_SIZE;
#  else
  stack_size = GC_get_hblk_size();
#  endif
  list_len = stack_size < MAX_LIST_LEN / LIST_LEN_PER_STACK
                 ? stack_size * LIST_LEN_PER_STACK
                 : MAX_LIST_LEN;
  printf("List of %lu nodes (initial mark stack of %lu entries)\n",
         (unsigned long)list_len, (unsigned long)stack_size);

  test_finalizer(0);
  GC_set_java_finalization(1);
  test_finalizer(1);
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
