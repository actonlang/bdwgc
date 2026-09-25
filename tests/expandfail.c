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
 * Check the sizes of the OS memory requests made by an automatic heap
 * expansion (`GC_collect_or_expand()`), including the fallback requests
 * made if the OS refuses the preceding ones.  This test includes the
 * collector source to intercept `GET_MEM()` (and refuse the requests on
 * demand), and to set the heap size (and the other variables the heap
 * expansion depends on) to synthetic values.
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

#define MAX_REQUESTS 4

/* The sizes of the recorded OS memory requests. */
static size_t requests[MAX_REQUESTS];
static unsigned request_count;

/* Whether to record the requests. */
static GC_bool record_requests;

/* The number of the subsequent requests to refuse. */
static unsigned requests_to_refuse;

static ptr_t
get_test_memory(size_t bytes)
{
  if (record_requests) {
    if (request_count < MAX_REQUESTS)
      requests[request_count] = bytes;
    request_count++;
    if (requests_to_refuse > 0) {
      requests_to_refuse--;
      return NULL;
    }
    /* Do not record the requests for the block headers and the like. */
    record_requests = FALSE;
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
#include "../extra/gc.c"
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

/*
 * Set the heap size, its part present at the latest forced unmapping and
 * the maximum heap size (zero means unlimited) to the given numbers of
 * blocks, set the free space divisor and the heap growth divisor, then
 * let an automatic heap expansion by `needed` blocks fail (refusing all
 * the OS memory requests) and check the requests.  `expected` is the
 * number of blocks of the first request (zero if no request is expected
 * at all), `fallback` is that of the second one, or zero if the second
 * request (if any) is the last resort one, of `needed` blocks.
 */
static void
check_growth(word heap_blocks, word excluded_blocks, word limit_blocks,
             word free_space_divisor, word growth_divisor, word needed,
             unsigned flags, word expected, word fallback)
{
  word old_heapsize = GC_heapsize;
  word old_excluded = GC_heapsize_at_forced_unmap;
  word old_limit = GC_max_heapsize;
  word old_free_space_divisor = GC_free_space_divisor;
  size_t expected_requests[MAX_REQUESTS];
  unsigned i, expected_count = 0;

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_dont_gc);
  if (expected != 0) {
    expected_requests[expected_count++]
        = ROUNDUP_PAGESIZE(expected * HBLKSIZE);
    if (fallback != 0)
      expected_requests[expected_count++]
          = ROUNDUP_PAGESIZE(fallback * HBLKSIZE);
    if (expected != needed)
      expected_requests[expected_count++]
          = ROUNDUP_PAGESIZE(needed * HBLKSIZE);
  }

  GC_heapsize = heap_blocks * HBLKSIZE;
  GC_heapsize_at_forced_unmap = excluded_blocks * HBLKSIZE;
  GC_max_heapsize = limit_blocks * HBLKSIZE;
  GC_free_space_divisor = free_space_divisor;
  GC_set_heap_growth_divisor(growth_divisor);
  request_count = 0;
  requests_to_refuse = ~0U;
  record_requests = TRUE;
  TEST_ASSERT(!GC_collect_or_expand(needed, flags, FALSE));
  record_requests = FALSE;
  GC_heapsize = old_heapsize;
  GC_heapsize_at_forced_unmap = old_excluded;
  GC_max_heapsize = old_limit;
  GC_free_space_divisor = old_free_space_divisor;
  GC_set_heap_growth_divisor(0);
  GC_alloc_fail_count = 0;

  for (i = 0; i < expected_count && i < request_count; ++i) {
    if (requests[i] != expected_requests[i])
      break;
  }
  if (i < expected_count || request_count != expected_count) {
    fprintf(stderr,
            "Heap of %lu blocks (%lu at forced unmap, limit %lu),"
            " free space divisor %lu, heap growth divisor %lu,"
            " %lu blocks needed (flags: %u), page size: %lu\n",
            (unsigned long)heap_blocks, (unsigned long)excluded_blocks,
            (unsigned long)limit_blocks, (unsigned long)free_space_divisor,
            (unsigned long)growth_divisor, (unsigned long)needed, flags,
            (unsigned long)GC_page_size);
    for (i = 0; i < request_count && i < MAX_REQUESTS; ++i)
      fprintf(stderr, "Requested %lu bytes\n", (unsigned long)requests[i]);
    for (i = 0; i < expected_count; ++i)
      fprintf(stderr, "Expected request of %lu bytes\n",
              (unsigned long)expected_requests[i]);
    exit(1);
  }
}

static void
check_limits(void)
{
  word heap_blocks = 32 * MAXHINCR;
  word large_needed = 4 * MAXHINCR;
  word max_blocks = divHBLKSZ(GC_WORD_MAX);
  word page_blocks = GC_page_size > HBLKSIZE ? divHBLKSZ(GC_page_size) : 1;
  /*
   * The black-list slop added to a request with `IGNORE_OFF_PAGE` flag
   * if the expansion limit is exceeded, and the resulting expansion for
   * a request of `MAXHINCR - 1` blocks without the flag (the slop is
   * twice `BL_LIMIT` then, but not more than the request).
   */
#ifdef NO_BLACK_LISTING
  word slop = 0;
  word normal_request = MAXHINCR;
#else
  word slop = 4;
  word bl_slop = 2 * divHBLKSZ(BL_LIMIT);
  word normal_request
      = MAXHINCR - 1 + (bl_slop < MAXHINCR - 1 ? bl_slop : MAXHINCR - 1);
#endif
#ifdef USE_MUNMAP
  unsigned old_unmap_threshold = GC_unmap_threshold;

  /* The synthetic values do not match the unmapped blocks, if any. */
  TEST_ASSERT(0 == GC_unmapped_bytes);
  /* Do not unmap blocks (`GC_unmap_old()`) before an expansion. */
  GC_unmap_threshold = 0;
#endif

  /*
   * With the zero heap growth divisor, the speculative part of
   * an increment is limited by `MAXHINCR` blocks.  If an expansion
   * fails, the one of the requested size is tried.
   */
  check_growth(0, 0, 0, 3, 0, 1, IGNORE_OFF_PAGE, MINHINCR, 0);
  check_growth(16 * MAXHINCR, 0, 0, 3, 0, 1, IGNORE_OFF_PAGE, MAXHINCR, 0);
  check_growth(heap_blocks, 0, 0, 3, 0, 1, IGNORE_OFF_PAGE, MAXHINCR, 0);
  check_growth(heap_blocks, 0, 0, 64, 0, 1, IGNORE_OFF_PAGE,
               heap_blocks / 64 + 1, 0);
  check_growth(heap_blocks, 0, 0, 3, 0, large_needed, IGNORE_OFF_PAGE,
               large_needed + slop, 0);
  check_growth(heap_blocks, 0, 0, 3, 0, MAXHINCR - 1, 0, normal_request, 0);

  /*
   * The product of a huge free space divisor and `HBLKSIZE` would wrap
   * around (to zero and to `HBLKSIZE` for the last cases).
   */
  check_growth(heap_blocks, 0, 0, GC_WORD_MAX, 0, 1, IGNORE_OFF_PAGE, MINHINCR,
               0);
  check_growth(heap_blocks, 0, 0, max_blocks + 1, 0, 1, IGNORE_OFF_PAGE,
               MINHINCR, 0);
  check_growth(heap_blocks, 0, 0, max_blocks + 2, 0, 1, IGNORE_OFF_PAGE,
               MINHINCR, 0);
  check_growth(heap_blocks, 0, 0, max_blocks + 2, 16, 1, IGNORE_OFF_PAGE,
               MINHINCR, 0);

  /*
   * With a nonzero heap growth divisor, the limit is the heap size
   * (excluding the part present at the latest forced unmapping) divided
   * by the value, if bigger than `MAXHINCR`.  If a scaled expansion
   * fails, the one limited by `MAXHINCR` is tried next.
   */
  check_growth(16 * MAXHINCR, 0, 0, 3, 16, 1, IGNORE_OFF_PAGE, MAXHINCR, 0);
  check_growth(heap_blocks, 0, 0, 3, 16, 1, IGNORE_OFF_PAGE, 2 * MAXHINCR,
               MAXHINCR);
  check_growth(heap_blocks, 16 * MAXHINCR, 0, 3, 16, 1, IGNORE_OFF_PAGE,
               MAXHINCR, 0);
  check_growth(heap_blocks, heap_blocks, 0, 3, 16, 1, IGNORE_OFF_PAGE,
               MINHINCR, 0);
  check_growth(heap_blocks, 0, 0, 64, 16, 1, IGNORE_OFF_PAGE,
               heap_blocks / 64 + 1, 0);
  check_growth(heap_blocks, 0, 0, 3, 16, large_needed, IGNORE_OFF_PAGE,
               large_needed + slop, 0);
  /* The fallback request keeps the black-list slop. */
  check_growth(heap_blocks, 0, 0, 3, 16, MAXHINCR - 1, 0, 2 * MAXHINCR,
               normal_request);
  /* The free space divisor still bounds the expansion. */
  check_growth(heap_blocks, 0, 0, 3, 1, 1, IGNORE_OFF_PAGE,
               heap_blocks / 3 + 1, MAXHINCR);
  /* A huge heap growth divisor is the same as zero one. */
  check_growth(heap_blocks, 0, 0, 3, GC_WORD_MAX, 1, IGNORE_OFF_PAGE, MAXHINCR,
               0);

  /*
   * The expansion is limited by the maximum heap size, and no request is
   * made if the size rounded up to a page exceeds the limit.
   */
  check_growth(heap_blocks, 0, heap_blocks + 8 * page_blocks, 3, 16, 1,
               IGNORE_OFF_PAGE, 8 * page_blocks, 0);
  if (page_blocks > 1)
    check_growth(heap_blocks, 0, heap_blocks + page_blocks - 1, 3, 16, 1,
                 IGNORE_OFF_PAGE, 0, 0);
  check_growth(heap_blocks, 0, heap_blocks, 3, 16, 1, IGNORE_OFF_PAGE, 0, 0);

  /* The expansion size is saturated at the maximum number of blocks. */
  check_growth(max_blocks, 0, 0, 1, 16, max_blocks, IGNORE_OFF_PAGE,
               max_blocks, 0);
#ifdef USE_MUNMAP
  GC_unmap_threshold = old_unmap_threshold;
#endif
}

/* Check the fallback requests succeed for the real heap. */
static void
check_real_growth(void)
{
  word old_free_space_divisor = GC_free_space_divisor;
  word heapsize = GC_heapsize;
  word heap_blocks = divHBLKSZ(GC_heapsize - GC_heapsize_at_forced_unmap);

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_dont_gc);
  TEST_ASSERT(heap_blocks > MAXHINCR);

  /*
   * The whole heap is requested with the heap growth divisor of 1 (and
   * the free space divisor of 1); refuse it.
   */
  GC_free_space_divisor = 1;
  GC_set_heap_growth_divisor(1);
  request_count = 0;
  requests_to_refuse = 1;
  record_requests = TRUE;
  TEST_ASSERT(GC_collect_or_expand(1, IGNORE_OFF_PAGE, FALSE));
  TEST_ASSERT(!record_requests);
  TEST_ASSERT(2 == request_count);
  TEST_ASSERT(requests[0] == ROUNDUP_PAGESIZE(heap_blocks * HBLKSIZE));
  TEST_ASSERT(requests[1] == ROUNDUP_PAGESIZE(MAXHINCR * HBLKSIZE));
  TEST_ASSERT(GC_heapsize == heapsize + requests[1]);
  /* The heap is not expanded again before the next collection. */
  TEST_ASSERT(GC_last_heap_growth_gc_no == GC_gc_no);

  /* Without the heap growth divisor, the request is the last resort. */
  heapsize = GC_heapsize;
  GC_set_heap_growth_divisor(0);
  request_count = 0;
  requests_to_refuse = 1;
  record_requests = TRUE;
  TEST_ASSERT(GC_collect_or_expand(1, IGNORE_OFF_PAGE, FALSE));
  TEST_ASSERT(!record_requests);
  TEST_ASSERT(2 == request_count);
  TEST_ASSERT(requests[0] == ROUNDUP_PAGESIZE(MAXHINCR * HBLKSIZE));
  TEST_ASSERT(requests[1] == ROUNDUP_PAGESIZE(HBLKSIZE));
  TEST_ASSERT(GC_heapsize == heapsize + requests[1]);
  GC_free_space_divisor = old_free_space_divisor;
}

int
main(void)
{
  word real_page_size;
  size_t old_min_bytes_allocd;

  GC_set_markers_count(1);
  GC_INIT();
  GC_set_warn_proc(GC_ignore_warn_proc);
  /* `GC_MAXIMUM_HEAP_SIZE` environment variable sets nonzero retries. */
  GC_set_max_retries(0);
  GC_set_max_heap_size(0);
  GC_disable();
  /* Make the heap bigger than `MAXHINCR` blocks for the real expansions. */
  TEST_ASSERT(GC_expand_hp(MAXHINCR * HBLKSIZE));

  LOCK();
  check_limits();
  /* No request could succeed while the synthetic page size is used. */
  real_page_size = GC_page_size;
  if (GC_page_size <= HBLKSIZE) {
    GC_page_size = 4 * HBLKSIZE;
    check_limits();
    GC_page_size = real_page_size;
  }
  check_real_growth();
  UNLOCK();

  /*
   * The expected heap growth (used to extend the plausible heap bounds)
   * is saturated if the minimum number of bytes allocated between
   * collections is huge.
   */
  old_min_bytes_allocd = GC_get_min_bytes_allocd();
  GC_set_min_bytes_allocd(~(size_t)0);
  GC_set_heap_growth_divisor(1);
  TEST_ASSERT(GC_expand_hp(2 * HBLKSIZE));
  GC_set_heap_growth_divisor(0);
  GC_set_min_bytes_allocd(old_min_bytes_allocd);

  GC_enable();
  GC_gcollect();
  printf("SUCCEEDED\n");
  return 0;
}
