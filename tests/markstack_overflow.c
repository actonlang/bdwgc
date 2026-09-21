/*
 * White-box companion to markstack.py, which supplies diagnostic-only hooks.
 * This is not linked against an installed or production collector.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 * Permission to use, copy, modify and distribute this program is granted,
 * provided this notice is retained in all copies.
 */

#include "gc.h"
#include "gc/gc_mark.h"
#include "gc/gc_typed.h"

#include <stdio.h>
#include <stdlib.h>

#define ARRAYS 1152
#define ELEMENTS 3072
#define STRIDE (8 * sizeof(void *))
#define CHECK(e)                                                        \
  do {                                                                 \
    if (!(e)) {                                                        \
      fprintf(stderr, "overflow: %s:%d: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

/* Provided only by the script's instrumented source copy. */
extern void GC_recovery_test_arm(unsigned);
extern void GC_recovery_test_snapshot(size_t *);

static void **roots;

static void
construct(void)
{
  GC_word bitmap = 1;
  GC_descr descriptor = GC_make_descriptor(&bitmap, 1);
  unsigned i, j;

  roots = (void **)GC_malloc(ARRAYS * sizeof(*roots));
  CHECK(roots != NULL);
  for (i = 0; i < ARRAYS; ++i) {
    char *p = (char *)GC_calloc_explicitly_typed(ELEMENTS, STRIDE, descriptor);

    CHECK(p != NULL);
    for (j = 0; j < 3; ++j) {
      unsigned slot = j == 2 ? ELEMENTS - 1 : j * (ELEMENTS / 2);
      GC_word *leaf = (GC_word *)GC_malloc_atomic(sizeof(*leaf));

      CHECK(leaf != NULL);
      *leaf = i * 3 + j + 1;
      *(void **)(p + slot * STRIDE) = leaf;
    }
    roots[i] = p;
  }
}

static void *GC_CALLBACK
check_live(void *unused)
{
  unsigned i, j;

  (void)unused;
  CHECK(GC_is_marked(roots));
  for (i = 0; i < ARRAYS; ++i) {
    char *p = (char *)roots[i];

    CHECK(p != NULL && GC_is_marked(p));
    for (j = 0; j < 3; ++j) {
      unsigned slot = j == 2 ? ELEMENTS - 1 : j * (ELEMENTS / 2);
      GC_word *leaf = *(GC_word **)(p + slot * STRIDE);

      CHECK(leaf != NULL && GC_is_marked(leaf) && *leaf == i * 3 + j + 1);
    }
  }
  return NULL;
}

int
main(int argc, char **argv)
{
  size_t initial[5], state[5];
  unsigned cycle, policy, markers;

  CHECK(argc == 3);
  policy = (unsigned)atoi(argv[1]); /* 0: no failure; 1: once; 2: always */
  markers = (unsigned)atoi(argv[2]);
  CHECK(policy <= 2 && (markers == 1 || markers == 4));
  GC_set_markers_count(markers);
  GC_INIT();
  GC_start_mark_threads();
  CHECK((unsigned)(GC_get_parallel() + 1) == markers);
  GC_set_disable_automatic_collection(1);
  GC_set_max_heap_size((size_t)768 * 1024 * 1024);
  GC_disable();
  construct();
  (void)GC_clear_stack(NULL);
  GC_recovery_test_arm(policy);
  GC_recovery_test_snapshot(initial);
  GC_enable();
  for (cycle = 0; cycle < 2; ++cycle) {
    GC_gcollect();
    (void)GC_call_with_alloc_lock(check_live, NULL);
    GC_recovery_test_snapshot(state);
    /* At most the original root-marking growth, never repeated batch growth. */
    CHECK(state[0] <= initial[0] * ((size_t)1 << (cycle + 1)));
    if (policy == 2)
      CHECK(state[0] == initial[0] && state[1] == state[2]);
    (void)GC_clear_stack(NULL);
  }
  if (markers == 4) {
    CHECK(state[3] > 0); /* The adversary really overflowed. */
    if (policy != 0)
      CHECK(state[2] > 0); /* Failure injection was not merely armed. */
  }
  if (policy == 1 && state[1] != 0)
    CHECK(state[2] == 1);
  printf("{\"markers\":%u,\"policy\":%u,\"initial_entries\":%lu,"
         "\"final_entries\":%lu,\"growth_attempts\":%lu,"
         "\"injected_failures\":%lu,\"copyback_overflows\":%lu,"
         "\"entry_bytes\":%lu,\"verified_cycles\":2}\n",
         markers, policy, (unsigned long)initial[0], (unsigned long)state[0],
         (unsigned long)state[1], (unsigned long)state[2],
         (unsigned long)state[3], (unsigned long)state[4]);
  return 0;
}
