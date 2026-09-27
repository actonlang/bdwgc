/*
 * Test whether `GC_realloc` frees the original object after moving it,
 * both in the default mode and in the "no free on realloc" mode.
 * The objects are allocated by the non-debug functions even if `GC_DEBUG`
 * is defined, as `GC_realloc` expects them.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#ifndef GC_IGNORE_WARN
/* Ignore misleading "Out of Memory!" warnings. */
#  define GC_IGNORE_WARN
#endif

#include "gc.h"

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/*
 * The size of a small object, i.e. not bigger than a half of the smallest
 * supported heap block (512 bytes).
 */
#define SMALL_LB 200

/*
 * The size of a big object, i.e. bigger than a half of the biggest
 * supported heap block (64 KB).
 */
#define BIG_LB 40000

#ifdef IGNORE_FREE
/* `GC_realloc` never frees the original object in this configuration. */
#  define EXPECTED_FREE(e) 0
#else
#  define EXPECTED_FREE(e) (e)
#endif

/* The number of reallocations in the loop checking the heap growth. */
#ifndef N_LOOP_REALLOCS
#  define N_LOOP_REALLOCS 1000000
#endif

static void *
checked(void *p)
{
  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}

static void
fill(void *p, size_t lb)
{
  size_t i;

  for (i = 0; i < lb; i++)
    ((unsigned char *)p)[i] = (unsigned char)(i * 7 + 1);
}

static int
is_filled(const void *p, size_t lb)
{
  size_t i;

  for (i = 0; i < lb; i++) {
    if (((const unsigned char *)p)[i] != (unsigned char)(i * 7 + 1))
      return 0;
  }
  return 1;
}

/*
 * Fill the object `p` of `old_lb` bytes, reallocate it to `new_lb`
 * bytes, check that the object is moved with its contents, and return
 * whether the original object has been freed, as the collector
 * statistics reports.  The new object is freed if `uncollectable`.
 * The collections should be disabled by the caller.
 */
static int
realloc_frees(void *p, size_t old_lb, size_t new_lb, int uncollectable)
{
  size_t freed_bytes = GC_get_expl_freed_bytes_since_gc();
  void *q;

  fill(p, old_lb);
  q = checked(GC_realloc(p, new_lb));
  CHECK(q != p);
  CHECK(is_filled(q, old_lb < new_lb ? old_lb : new_lb));
  if (GC_get_expl_freed_bytes_since_gc() == freed_bytes) {
    /* The original object is intact, i.e. not on a free list. */
    CHECK(is_filled(p, old_lb));
    return 0;
  }
  if (uncollectable)
    GC_free(q);
  return 1;
}

int
main(void)
{
  const char *str;
  int no_free;
  int keep_free;
  size_t freed_bytes;
  const void *volatile last = NULL;
  unsigned long i;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  /* Check the initial mode. */
  str = getenv("GC_REALLOC_NO_FREE");
  if (str != NULL) {
    no_free = !(str[0] == '0' && str[1] == '\0');
  } else {
#ifdef GC_REALLOC_NO_FREE
    no_free = 1;
#else
    no_free = 0;
#endif
  }
  CHECK(GC_get_realloc_no_free() == no_free);
  printf("Initial no-free-on-realloc mode: %d\n", no_free);

  GC_disable();
  for (no_free = 0; no_free <= 1; no_free++) {
    /* In the leak detection mode, everything is freed. */
    keep_free = EXPECTED_FREE(!no_free || GC_get_find_leak());
    GC_set_realloc_no_free(no_free);
    CHECK(GC_get_realloc_no_free() == no_free);

    /* A small collectable object is freed unless the mode is on. */
    CHECK(realloc_frees(checked(GC_malloc(16)), 16, SMALL_LB, 0) == keep_free);
    CHECK(realloc_frees(checked(GC_malloc_atomic(16)), 16, SMALL_LB, 0)
          == keep_free);
    /* Shrinking a small object to less than a half moves it too. */
    CHECK(realloc_frees(checked(GC_malloc(SMALL_LB)), SMALL_LB, 16, 0)
          == keep_free);

    /* Uncollectable and big objects are always freed. */
    CHECK(realloc_frees(checked(GC_malloc_uncollectable(16)), 16, SMALL_LB, 1)
          == EXPECTED_FREE(1));
    CHECK(realloc_frees(checked(GC_malloc(BIG_LB)), BIG_LB, 2 * BIG_LB, 0)
          == EXPECTED_FREE(1));
    CHECK(realloc_frees(checked(GC_malloc(BIG_LB)), BIG_LB, 16, 0)
          == EXPECTED_FREE(1));
    CHECK(
        realloc_frees(checked(GC_malloc_atomic(BIG_LB)), BIG_LB, 2 * BIG_LB, 0)
        == EXPECTED_FREE(1));
    CHECK(realloc_frees(checked(GC_malloc_uncollectable(BIG_LB)), BIG_LB,
                        2 * BIG_LB, 1)
          == EXPECTED_FREE(1));

    /* A reallocation to zero size is equivalent to `GC_free`. */
    freed_bytes = GC_get_expl_freed_bytes_since_gc();
    CHECK(NULL == GC_realloc(checked(GC_malloc(16)), 0));
    CHECK((GC_get_expl_freed_bytes_since_gc() > freed_bytes)
          == EXPECTED_FREE(1));
  }
  GC_enable();

  /* The objects left to the collector are reclaimed. */
  for (i = 0; i < N_LOOP_REALLOCS; i++) {
    void *p = checked(GC_malloc(16));

    last = checked(GC_realloc(p, SMALL_LB));
  }
  printf("Heap size after %lu reallocations: %lu KiB\n",
         (unsigned long)N_LOOP_REALLOCS,
         (unsigned long)(GC_get_heap_size() >> 10));
  if (!GC_is_disabled()) {
    CHECK(GC_get_heap_size() < (size_t)N_LOOP_REALLOCS * (16 + SMALL_LB) / 8);
  }
  CHECK(last != NULL);
  printf("SUCCEEDED\n");
  return 0;
}
