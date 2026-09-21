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

#include "gc.h"
#include "gc/gc_mark.h"
#include "gc/gc_typed.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef GC_TEST_MARK_STACK_GROWTH
/* Supplied only by markstack.py's instrumented source copy. */
extern void GC_recovery_test_arm(unsigned);
extern void GC_recovery_test_snapshot(size_t *);
static size_t initial_stack[5], final_stack[5];
#endif

#define CHECK(e)                                                        \
  do {                                                                 \
    if (!(e)) {                                                        \
      fprintf(stderr, "markstack: %s:%d: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

#define NODES 65536
#define ARRAYS 128
#define ELEMENTS 2048
#define STRIDE (8 * sizeof(void *))

struct node {
  struct node *next;
  struct node *edges[6];
  GC_word *leaf;
  GC_word id;
};

/* These are the only persistent roots for the two test graphs. */
static struct node *root;
static void **arrays;
static unsigned mark_started, mark_completed, stop_checks;
/* Only enabled during a custom-stop attempt, when helpers are disabled. */
static unsigned watch_target, target_scanned;
static GC_word random_state = 42;

static GC_word
next_random(void)
{
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static struct GC_ms_entry *GC_CALLBACK
mark_node(GC_word *address, struct GC_ms_entry *top,
          struct GC_ms_entry *limit, GC_word env)
{
  struct node *p = (struct node *)address;
  unsigned i;

  (void)env;
  top = GC_MARK_AND_PUSH(p->next, top, limit, (void **)&p->next);
  for (i = 0; i < 6; ++i)
    top = GC_MARK_AND_PUSH(p->edges[i], top, limit, (void **)&p->edges[i]);
  top = GC_MARK_AND_PUSH(p->leaf, top, limit, (void **)&p->leaf);
  if (watch_target && p == root)
    target_scanned = 1;
  return top;
}

static void
make_graphs(void)
{
  struct node **index = (struct node **)malloc(NODES * sizeof(*index));
  unsigned kind = GC_new_kind(GC_new_free_list(),
                             GC_MAKE_PROC(GC_new_proc(mark_node), 0), 0, 1);
  GC_word bitmap = 1;
  GC_descr descriptor = GC_make_descriptor(&bitmap, 1);
  unsigned i, j;

  CHECK(index != NULL);
  for (i = 0; i < NODES; ++i) {
    index[i] = (struct node *)GC_generic_malloc(sizeof(**index), (int)kind);
    CHECK(index[i] != NULL);
    index[i]->id = i + 1;
  }
  for (i = 0; i < NODES; ++i) {
    index[i]->next = i + 1 < NODES ? index[i + 1] : NULL;
    for (j = 0; j < 6; ++j)
      index[i]->edges[j] = index[next_random() % NODES];
  }
  root = index[0];
  for (i = 0; i < NODES; ++i)
    ((struct node *volatile *)index)[i] = NULL;
  free(index);

  arrays = (void **)GC_malloc(ARRAYS * sizeof(*arrays));
  CHECK(arrays != NULL);
  for (i = 0; i < ARRAYS; ++i) {
    char *p = (char *)GC_calloc_explicitly_typed(ELEMENTS, STRIDE, descriptor);

    CHECK(p != NULL);
    /* Sparse descriptors expand into many work items even for zero data. */
    for (j = 0; j < 3; ++j) {
      unsigned slot = j == 2 ? ELEMENTS - 1 : j * (ELEMENTS / 2);
      GC_word *leaf = (GC_word *)GC_malloc_atomic(sizeof(*leaf));

      CHECK(leaf != NULL);
      *leaf = i * 3 + j + 1;
      *(void **)(p + slot * STRIDE) = leaf;
    }
    arrays[i] = p;
  }
}

static void *GC_CALLBACK
check_graphs(void *unused)
{
  struct node *p = root;
  unsigned i, j;

  (void)unused;
  for (i = 0; i < NODES; ++i) {
    CHECK(p != NULL && GC_is_marked(p) && p->id == i + 1);
    for (j = 0; j < 6; ++j)
      CHECK(p->edges[j] != NULL && GC_is_marked(p->edges[j])
            && p->edges[j]->id > 0 && p->edges[j]->id <= NODES);
    if (p->leaf != NULL)
      CHECK(GC_is_marked(p->leaf) && *p->leaf == p->id);
    p = p->next;
  }
  CHECK(p == NULL && GC_is_marked(arrays));
  for (i = 0; i < ARRAYS; ++i) {
    char *a = (char *)arrays[i];

    CHECK(a != NULL && GC_is_marked(a));
    for (j = 0; j < 3; ++j) {
      unsigned slot = j == 2 ? ELEMENTS - 1 : j * (ELEMENTS / 2);
      GC_word *leaf = *(GC_word **)(a + slot * STRIDE);

      CHECK(leaf != NULL && GC_is_marked(leaf) && *leaf == i * 3 + j + 1);
    }
  }
  return NULL;
}

static void
mutate_graph(void)
{
  struct node *p = root;
  unsigned i;

  for (i = 0; i < 128; ++i, p = p->next) {
    GC_word *leaf = (GC_word *)GC_malloc_atomic(sizeof(*leaf));

    CHECK(leaf != NULL);
    *leaf = p->id;
    GC_PTR_STORE_AND_DIRTY(&p->leaf, leaf);
  }
}

static void GC_CALLBACK
collection_event(GC_EventType event)
{
  if (event == GC_EVENT_MARK_START)
    mark_started = 1;
  else if (event == GC_EVENT_MARK_END)
    ++mark_completed;
}

static int GC_CALLBACK
abort_mark(void)
{
  return mark_started && ++stop_checks >= 8 && target_scanned;
}

static int GC_CALLBACK
custom_continue(void)
{
  ++stop_checks;
  return 0;
}

int
main(int argc, char **argv)
{
  unsigned round;

  CHECK(argc == 1 || (argc == 2 && argv[1][0] == '1' && argv[1][1] == '\0'));
#ifdef GC_THREADS
  GC_set_markers_count(argc == 2 ? 1 : 4);
#else
  (void)argv;
#endif
  GC_INIT();
#ifdef GC_THREADS
  GC_start_mark_threads();
#endif
  if (argc == 2)
    CHECK(GC_get_parallel() == 0);
  GC_set_disable_automatic_collection(1);
  GC_set_on_collection_event(collection_event);
  GC_disable();
  make_graphs();
#ifdef GC_TEST_MARK_STACK_GROWTH
  GC_recovery_test_arm(2); /* Fail every subsequent stack growth. */
  GC_recovery_test_snapshot(initial_stack);
#endif
  GC_enable();
  for (round = 0; round < 2; ++round) {
    GC_gcollect();
    (void)GC_call_with_alloc_lock(check_graphs, NULL);
    mark_started = stop_checks = target_scanned = 0;
    watch_target = 1;
    CHECK(!GC_try_to_collect(abort_mark) && target_scanned);
    watch_target = 0;
    mutate_graph();
    (void)GC_clear_stack(NULL);
    stop_checks = 0;
    CHECK(GC_try_to_collect(custom_continue) && stop_checks != 0);
    (void)GC_call_with_alloc_lock(check_graphs, NULL);
  }

  GC_enable_incremental();
  if (GC_is_incremental_mode()) {
    for (round = 0; round < 2; ++round) {
      unsigned steps = 0;
      unsigned completed = mark_completed;

      GC_set_time_limit(round == 0 ? 0 : GC_TIME_UNLIMITED);
      mark_started = stop_checks = target_scanned = 0;
      watch_target = 1;
      CHECK(!GC_try_to_collect(abort_mark) && target_scanned);
      watch_target = 0;
      /* The root's old fields were scanned before this old-to-young store. */
      mutate_graph();
      (void)GC_clear_stack(NULL);
      while (GC_collect_a_little()) {
        CHECK(++steps < 1000000);
        if (steps == 1) {
          mutate_graph();
          (void)GC_clear_stack(NULL);
        }
      }
      CHECK(mark_completed > completed);
      (void)GC_call_with_alloc_lock(check_graphs, NULL);
      completed = mark_completed;
      mutate_graph();
      (void)GC_clear_stack(NULL);
      GC_start_incremental_collection();
      steps = 0;
      while (GC_collect_a_little())
        CHECK(++steps < 1000000);
      CHECK(mark_completed > completed);
      (void)GC_call_with_alloc_lock(check_graphs, NULL);
    }
  } else {
#ifdef GC_TEST_MARK_STACK_GROWTH
    CHECK(!"Incremental marking required for failure-injection coverage");
#endif
    puts("Incremental marking unavailable; skipped incremental checks.");
  }
#ifdef GC_TEST_MARK_STACK_GROWTH
  GC_recovery_test_snapshot(final_stack);
  CHECK(final_stack[0] == initial_stack[0]);
  if (GC_get_parallel())
    CHECK(final_stack[2] > 0 && final_stack[3] > 0);
  printf("{\"markers\":%d,\"initial_entries\":%lu,\"final_entries\":%lu,"
         "\"growth_attempts\":%lu,\"injected_failures\":%lu,"
         "\"copyback_overflows\":%lu,\"scanned_target_resume\":true}\n",
         GC_get_parallel() + 1, (unsigned long)initial_stack[0],
         (unsigned long)final_stack[0], (unsigned long)final_stack[1],
         (unsigned long)final_stack[2], (unsigned long)final_stack[3]);
#else
  puts("SUCCEEDED");
#endif
  return 0;
}
