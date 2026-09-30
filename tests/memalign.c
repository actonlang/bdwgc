/*
 * Test that the collector reclaims (and clears the free list of) the
 * small objects allocated by `GC_memalign()` with an alignment bigger than
 * the granule, if these are the first objects of their kind (i.e., there
 * is no call of `GC_malloc()` and friends before).
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"
#include "gc/gc_tiny_fl.h"

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define N_ROUNDS 10

/* An alignment bigger than the granule, and a small size. */
#define ALIGNMENT (4 * GC_GRANULE_BYTES)
#define OBJ_SIZE (3 * GC_GRANULE_BYTES)

/*
 * Allocate an aligned object, check that it is a valid one, and deallocate
 * it explicitly (thus putting it to the global free list of the kind).
 */
static void
alloc_check_and_free(void)
{
  void *p = GC_memalign(ALIGNMENT, OBJ_SIZE);

  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  CHECK(GC_base(p) == p);
  CHECK(((GC_uintptr_t)p & (ALIGNMENT - 1)) == 0);
  GC_free(p);
}

/*
 * Overwrite the stack below the caller frame, so that the collector does
 * not find the pointer to the deallocated object there (a marked object
 * keeps its block from being returned to the heap).  Each element is
 * stored through a `volatile` lvalue, since the compiler may remove other
 * stores (e.g., by `memset()`) to an array that is not read afterwards.
 */
static void
clear_stack(void)
{
  volatile char buf[8192];
  size_t i;

  for (i = 0; i < sizeof(buf); i++)
    buf[i] = 0;
}

/* Called indirectly to prevent inlining. */
static void (*volatile alloc_check_and_free_fn)(void) = alloc_check_and_free;
static void (*volatile clear_stack_fn)(void) = clear_stack;

int
main(void)
{
  int i;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  for (i = 0; i < N_ROUNDS; i++) {
    /*
     * The object deallocated in the previous round is unmarked, thus
     * a collection should remove it from the free list (and return its
     * block to the heap) instead of letting it be allocated again.
     */
    alloc_check_and_free_fn();
    clear_stack_fn();
    GC_gcollect();
  }
  printf("SUCCEEDED\n");
  return 0;
}
