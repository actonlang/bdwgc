/*
 * Test that the first allocation of each small object size succeeds even
 * if the sizes are requested in the descending order.  The entries of
 * `GC_size_map` for the sizes bigger than those of the tiny free lists
 * are filled in on demand, a range of the sizes around the requested one
 * at once.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/*
 * The size of the first allocated object.  It covers all the sizes of
 * small objects if the heap block size is 8 KB at most.
 */
#ifndef MAX_LB
#  define MAX_LB 4096
#endif

int
main(void)
{
  size_t lb;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  for (lb = MAX_LB; lb > 0; lb--) {
    const void *p = GC_malloc(lb);

    CHECK(p != NULL);
    CHECK(GC_size(p) >= lb);
  }
  printf("SUCCEEDED\n");
  return 0;
}
