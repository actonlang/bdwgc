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
 * Test that the maximum heap increment scales with the heap size if
 * the heap growth divisor is set by `GC_set_heap_growth_divisor()`.
 * The heap is expanded by allocating atomic objects which are never
 * written, thus the test needs some address space but little memory.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"

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

/* The heap growth divisor to test. */
#define GROWTH_DIVISOR 4

/*
 * The size limits of the objects used to fill the heap.  The objects
 * should be small compared to the default maximum heap increment (which
 * is at least 2 MiB), so that the size of the request which causes
 * a heap expansion does not exceed the maximum.
 */
#define MIN_FILL_BYTES (64 * 1024)
#define MAX_FILL_BYTES (256 * 1024)

/*
 * The number of heap resizes and the latest heap size reported.  These are
 * updated by the callback during an allocation call, thus `volatile`: the
 * compiler may assume that the allocation functions do not modify any
 * global variable (`GC_ATTR_MALLOC` includes `__declspec(noalias)` for
 * MS VC, and for clang targeting MS VC).
 */
static volatile unsigned resize_count;
static volatile GC_word last_heap_size;

static void GC_CALLBACK
on_heap_resize(GC_word new_size)
{
  last_heap_size = new_size;
  resize_count = resize_count + 1;
}

/* Return the heap size including the unmapped part. */
static GC_word
heap_size_full(void)
{
  GC_word heap_size, unmapped_bytes;

  GC_get_heap_usage_safe(&heap_size, NULL, &unmapped_bytes, NULL, NULL);
  return heap_size + unmapped_bytes;
}

/*
 * Allocate objects until the heap is expanded (automatically) once.
 * Return the increment, the heap size before the expansion (in `*pheap`)
 * and the size of the objects (in `*plb`).
 */
static GC_word
expand_heap(GC_word *pheap, size_t *plb)
{
  GC_word heap = heap_size_full();
  unsigned count = resize_count;
  size_t lb = (size_t)(heap / 64);

  if (lb < MIN_FILL_BYTES) {
    lb = MIN_FILL_BYTES;
  } else if (lb > MAX_FILL_BYTES) {
    lb = MAX_FILL_BYTES;
  }
  do {
    const void *p = GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE(lb);

    CHECK_OUT_OF_MEMORY(p);
  } while (resize_count == count);
  TEST_ASSERT(resize_count == count + 1);
  TEST_ASSERT(last_heap_size > heap);
  *pheap = heap;
  *plb = lb;
  return last_heap_size - heap;
}

/*
 * Check the heap increment is about the expected one (allowing for
 * the rounding to heap block and page boundaries).
 */
static void
check_increment(const char *what, GC_word increment, GC_word heap,
                GC_word expected, GC_word tolerance)
{
  if (increment + tolerance < expected || increment > expected + tolerance) {
    fprintf(stderr,
            "%s heap increment is %lu KiB, expected about %lu KiB"
            " (heap size: %lu KiB)\n",
            what, (unsigned long)(increment >> 10),
            (unsigned long)(expected >> 10), (unsigned long)(heap >> 10));
    exit(1);
  }
}

int
main(void)
{
  GC_word default_increment, increment, heap, tolerance;
  size_t lb;
  int i;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  /*
   * Override the settings (possibly set by the environment variables)
   * the heap increment depends on.  The free space divisor of 1 lets
   * the heap grow up to twice its size (before the limit is applied).
   * The collector is disabled, so that an allocation failure expands
   * the heap at once; the increment does not depend on whether
   * a collection has been done before.
   */
  GC_set_max_heap_size(0);
  GC_set_free_space_divisor(1);
  GC_set_heap_growth_divisor(0);
  TEST_ASSERT(GC_get_heap_growth_divisor() == 0);
  GC_set_on_heap_resize(on_heap_resize);
  GC_disable();

  /*
   * Grow the heap until an increment is smaller than the heap size,
   * i.e. the increment is limited by the default maximum one.
   */
  for (i = 0;; i++) {
    TEST_ASSERT(i < 24);
    increment = expand_heap(&heap, &lb);
    if (increment < heap)
      break;
  }
  default_increment = increment;
  tolerance = default_increment / 16;

  /*
   * Make the heap large enough for the scaled increment to be at least
   * twice the default one.
   */
  heap = heap_size_full();
  if (heap < 2 * GROWTH_DIVISOR * default_increment) {
    size_t bytes = (size_t)(2 * GROWTH_DIVISOR * default_increment - heap);

    TEST_ASSERT(GC_expand_hp(bytes));
  }

  GC_set_heap_growth_divisor(GROWTH_DIVISOR);
  TEST_ASSERT(GC_get_heap_growth_divisor() == GROWTH_DIVISOR);
  increment = expand_heap(&heap, &lb);
  check_increment("Scaled", increment, heap, heap / GROWTH_DIVISOR, tolerance);
  TEST_ASSERT(increment > default_increment);

  /*
   * The heap growth is still bounded by the free space divisor:
   * the heap size divided by it plus the requested size.
   */
  GC_set_free_space_divisor(2 * GROWTH_DIVISOR);
  increment = expand_heap(&heap, &lb);
  check_increment("Bounded", increment, heap, heap / (2 * GROWTH_DIVISOR) + lb,
                  tolerance);

  /* Zero divisor restores the default maximum increment. */
  GC_set_free_space_divisor(1);
  GC_set_heap_growth_divisor(0);
  increment = expand_heap(&heap, &lb);
  check_increment("Default", increment, heap, default_increment, 0);

  printf("SUCCEEDED\n");
  return 0;
}
