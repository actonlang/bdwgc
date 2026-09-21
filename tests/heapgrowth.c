/*
 * Exercise automatic growth limits without reserving a huge address space.
 * Include the collector so allocation failure can be injected at GET_MEM.
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

static GC_bool reject_memory;
static GC_bool fail_once;
static size_t requests[4];
static unsigned request_count;

static ptr_t
get_test_memory(size_t bytes)
{
  if (reject_memory) {
    if (request_count < sizeof(requests) / sizeof(requests[0]))
      requests[request_count] = bytes;
    ++request_count;
    if (fail_once)
      reject_memory = FALSE;
    return NULL;
  }
  return (ptr_t)GET_MEM(bytes);
}
#undef GET_MEM
#define GET_MEM(bytes) get_test_memory(bytes)
#include "../extra/gc.c"

#define CHECK(e)                                              \
  do {                                                        \
    if (!(e)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                \
    }                                                         \
  } while (0)

static void GC_CALLBACK
ignore_warning(const char *msg, GC_word arg)
{
  (void)msg;
  (void)arg;
}

/*
 * All attempts fail before changing any heap section.  Restore the synthetic
 * counters before enabling collection again.  The second request checks the
 * fallback to the exact allocation size after speculative growth fails.
 */
static void
check_growth(word heap_blocks, word excluded_blocks, word divisor, word needed,
             word limit_blocks, word expected)
{
  word old_heap = GC_heapsize;
  word old_excluded = GC_heapsize_at_forced_unmap;
  word old_limit = GC_max_heapsize;
  word old_divisor = GC_free_space_divisor;
#ifdef USE_MUNMAP
  unsigned old_unmap = GC_unmap_threshold;
  GC_unmap_threshold = 0;
#endif

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_dont_gc);
  GC_heapsize = heap_blocks * HBLKSIZE;
  GC_heapsize_at_forced_unmap = excluded_blocks * HBLKSIZE;
  GC_max_heapsize = limit_blocks * HBLKSIZE;
  GC_free_space_divisor = divisor;
  request_count = 0;
  reject_memory = TRUE;
  CHECK(!GC_collect_or_expand(needed, IGNORE_OFF_PAGE, FALSE));
  reject_memory = FALSE;
  if (expected == 0) {
    CHECK(request_count == 0);
  } else {
    CHECK(request_count == (expected == needed ? 1u : 2u));
    CHECK(requests[0] == ROUNDUP_PAGESIZE(expected * HBLKSIZE));
    CHECK(requests[request_count - 1] == ROUNDUP_PAGESIZE(needed * HBLKSIZE));
  }
  GC_heapsize = old_heap;
  GC_heapsize_at_forced_unmap = old_excluded;
  GC_max_heapsize = old_limit;
  GC_free_space_divisor = old_divisor;
  GC_alloc_fail_count = 0;
#ifdef USE_MUNMAP
  GC_unmap_threshold = old_unmap;
#endif
}

static void *live[256];

static void
check_limits(void)
{
  word heap_blocks = 32 * MAXHINCR;
  word large_needed = 4 * MAXHINCR;
  word slop = 4;
  word max_blocks = divHBLKSZ(GC_WORD_MAX);
  word page_blocks = GC_page_size > HBLKSIZE ? divHBLKSZ(GC_page_size) : 1;

#ifdef NO_BLACK_LISTING
  slop = 0;
#endif
  check_growth(0, 0, 3, 1, 0, MINHINCR);
  check_growth(16 * MAXHINCR, 0, 3, 1, 0, MAXHINCR);
  check_growth(heap_blocks, 0, 3, 1, 0, 2 * MAXHINCR);
  check_growth(heap_blocks, 16 * MAXHINCR, 3, 1, 0, MAXHINCR);
  check_growth(heap_blocks, heap_blocks, 3, 1, 0, MINHINCR);
  check_growth(heap_blocks, 0, 64, 1, 0, heap_blocks / 64 + 1);
  check_growth(heap_blocks, 0, GC_WORD_MAX, 1, 0, MINHINCR);
  /* Multiplying this divisor by HBLKSIZE would wrap to zero. */
  check_growth(heap_blocks, 0, max_blocks + 1, 1, 0, MINHINCR);
  check_growth(heap_blocks, 0, 3, 1, heap_blocks + 8 * page_blocks,
               8 * page_blocks);
  if (page_blocks > 1)
    check_growth(heap_blocks, 0, 3, 1, heap_blocks + page_blocks - 1, 0);
  check_growth(heap_blocks, 0, 3, 1, heap_blocks, 0);
  check_growth(heap_blocks, 0, 3, large_needed, 0, large_needed + slop);
  check_growth(max_blocks, 0, 1, max_blocks, 0, max_blocks);
}

int
main(void)
{
  unsigned i;
  GC_word old_min;
  word real_page_size;

  GC_set_markers_count(1);
  GC_INIT();
  GC_set_warn_proc(ignore_warning);
  GC_disable();
  LOCK();
  check_limits();
  /* No OS request can succeed while using the synthetic page size. */
  real_page_size = GC_page_size;
  if (GC_page_size <= HBLKSIZE) {
    GC_page_size = 4 * HBLKSIZE;
    check_limits();
    GC_page_size = real_page_size;
  }
  /* A failed speculative request must still permit a small successful one. */
  reject_memory = TRUE;
  fail_once = TRUE;
  request_count = 0;
  CHECK(GC_collect_or_expand(1, IGNORE_OFF_PAGE, FALSE));
  CHECK(request_count == 1);
  CHECK(!reject_memory);
  CHECK(GC_last_heap_growth_gc_no == GC_gc_no);
  UNLOCK();
  GC_enable();
  LOCK();
  /* Expansion must still force a collection before another expansion. */
  CHECK(GC_should_collect());
  UNLOCK();

  /* Check real expansion and marking after the failure-injection cases. */
  old_min = GC_get_min_bytes_allocd();
  GC_set_min_bytes_allocd(GC_WORD_MAX);
  CHECK(GC_expand_hp(2 * HBLKSIZE));
  GC_set_min_bytes_allocd(old_min);
  for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
    unsigned char *p = (unsigned char *)GC_malloc_atomic(1024);
    CHECK(p != NULL);
    p[0] = (unsigned char)i;
    p[1023] = (unsigned char)(i ^ 0x55);
    live[i] = p;
  }
  GC_gcollect();
  for (i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
    unsigned char *p = (unsigned char *)live[i];
    CHECK(p[0] == (unsigned char)i);
    CHECK(p[1023] == (unsigned char)(i ^ 0x55));
  }
  GC_gcollect_and_unmap();
  CHECK(GC_expand_hp(2 * HBLKSIZE));
  GC_gcollect();
  puts("Heap growth limits, failure fallback and live objects passed");
  return 0;
}
