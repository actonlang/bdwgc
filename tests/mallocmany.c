/*
 * Test that the collector reclaims (and clears the free lists of) the
 * objects of a kind which is allocated only by `GC_generic_malloc_many()`
 * (i.e., never by `GC_malloc()` and friends).
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "gc/gc_inline.h"

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define N_ROUNDS 10

/*
 * Allocate a list of pointer-free objects by `GC_generic_malloc_many()`,
 * check that they are all valid objects, and deallocate them explicitly
 * (thus putting them to the global free list of the kind).
 */
static void
alloc_check_and_free_list(size_t lb_adjusted)
{
  void *p;
  void *next;

  GC_generic_malloc_many(lb_adjusted, GC_I_PTRFREE, &p);
  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  for (; p != NULL; p = next) {
    CHECK(GC_base(p) == p);
    CHECK(GC_size(p) >= lb_adjusted);
    next = GC_NEXT(p);
    GC_free(p);
  }
}

int
main(void)
{
  /* The size (a multiple of the granule) no other code allocates. */
  size_t lb_adjusted = 7 * GC_GRANULE_BYTES;
  int i;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  for (i = 0; i < N_ROUNDS; i++) {
    /*
     * The objects deallocated in the previous round are unmarked, thus
     * a collection should remove them from the free list (and return
     * their block to the heap).
     */
    alloc_check_and_free_list(lb_adjusted);
    GC_gcollect();
  }
  printf("SUCCEEDED\n");
  return 0;
}
