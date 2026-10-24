/*
 * Test heap growth in the incremental mode while other threads build
 * free lists.  Some threads add small heap sections, one per
 * `GC_expand_hp()` call, while the others allocate lists of small objects
 * of several sizes and check the lists they keep live.  In the parallel
 * marking mode, a thread refilling its thread-local free list sweeps the
 * blocks left by a collection without the allocator lock, and removes
 * their write protection, thus the virtual dirty bits implementation sees
 * the heap sections change meanwhile.  With the `userfaultfd` virtual
 * dirty bits, each section has to be registered before any of its pages
 * is protected or unprotected, otherwise the collector aborts.
 * The test is skipped in a build without parallel marking or without
 * thread-local allocation, where no thread of the test sweeps without the
 * allocator lock.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#undef GC_NO_THREAD_REDIRECTS
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(GC_PTHREADS) && !defined(GC_WIN32_PTHREADS) \
    && defined(AO_HAVE_load) && defined(AO_HAVE_store)  \
    && defined(PARALLEL_MARK) && defined(THREAD_LOCAL_ALLOC)

#  include <pthread.h>

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

#  ifndef N_ALLOCATORS
#    define N_ALLOCATORS 4
#  endif

#  ifndef N_GROWERS
#    define N_GROWERS 2
#  endif

/*
 * The number of the heap sections added by each grower thread.  Where
 * each chunk of memory which the collector gets from the system takes an
 * entry of a table of `MAX_HEAP_SECTS` entries (`USE_PROC_FOR_LIBRARIES`,
 * defined if `malloc` is redirected on Linux, and Windows), the growers
 * take at most half of the entries.
 */
#  ifndef N_SECTIONS
#    if (defined(ANY_MSWIN) || defined(USE_PROC_FOR_LIBRARIES)) \
        && MAX_HEAP_SECTS < 2 * N_GROWERS * 1000
#      define N_SECTIONS (MAX_HEAP_SECTS / (2 * N_GROWERS))
#    else
#      define N_SECTIONS 1000
#    endif
#  endif

#  define SECTION_BYTES ((size_t)64 * 1024)

#  define LIST_LEN 1000

/* The lists kept live by each allocator thread. */
#  define KEEP_LISTS 8

struct node {
  struct node *next;
  GC_word value;
};

static volatile AO_t stop_allocating = 0;

/*
 * Allocate a list of `n` nodes of various sizes (so that thread-local free
 * lists of several size classes are refilled).
 */
static struct node *
make_list(unsigned n, GC_word seed)
{
  struct node *head = NULL;
  unsigned i;

  for (i = 0; i < n; ++i) {
    struct node *p
        = (struct node *)GC_MALLOC(sizeof(struct node) + (i % 16) * 16);

    CHECK_OUT_OF_MEMORY(p);
    p->next = head;
    p->value = seed + i;
    head = p;
  }
  return head;
}

static void
check_list(const struct node *p, unsigned n, GC_word seed)
{
  unsigned i;

  for (i = n; i-- > 0; p = p->next) {
    TEST_ASSERT(p != NULL);
    TEST_ASSERT(p->value == seed + i);
  }
  TEST_ASSERT(NULL == p);
}

static void *
allocate(void *arg)
{
  struct node *lists[KEEP_LISTS] = { NULL };
  GC_word seed = (GC_word)(GC_uintptr_t)arg * 100000;
  unsigned i;

  for (i = 0; !AO_load(&stop_allocating); ++i) {
    unsigned k = i % KEEP_LISTS;

    if (lists[k] != NULL)
      check_list(lists[k], LIST_LEN, seed + k);
    lists[k] = make_list(LIST_LEN, seed + k);
  }
  return arg;
}

static void *
grow(void *arg)
{
  GC_word seed = (GC_word)(GC_uintptr_t)arg * 100000;
  int i;

  for (i = 0; i < N_SECTIONS; ++i) {
    struct node *l;

    TEST_ASSERT(GC_expand_hp(SECTION_BYTES));
    /* Let the other threads allocate between the additions. */
    l = make_list(LIST_LEN / 10, seed + (GC_word)i);
    check_list(l, LIST_LEN / 10, seed + (GC_word)i);
  }
  return arg;
}

static void
start_threads(pthread_t *th, int n, void *(*fn)(void *), int first_id)
{
  int i;

  for (i = 0; i < n; ++i) {
    int err = pthread_create(&th[i], NULL, fn,
                             (void *)(GC_uintptr_t)(first_id + i));

    if (err != 0) {
      fprintf(stderr, "Thread #%d creation failed, errno= %d\n", first_id + i,
              err);
      exit(69);
    }
  }
}

int
main(void)
{
  pthread_t allocators[N_ALLOCATORS];
  pthread_t growers[N_GROWERS];
  int i;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    printf("test skipped\n");
    return 0;
  }
  GC_enable_incremental();
  if (!GC_is_incremental_mode()) {
    printf("test skipped (no incremental mode)\n");
    return 0;
  }

  start_threads(allocators, N_ALLOCATORS, allocate, 0);
  start_threads(growers, N_GROWERS, grow, N_ALLOCATORS);
  for (i = 0; i < N_GROWERS; ++i) {
    TEST_ASSERT(pthread_join(growers[i], NULL) == 0);
  }
  AO_store(&stop_allocating, 1);
  for (i = 0; i < N_ALLOCATORS; ++i) {
    TEST_ASSERT(pthread_join(allocators[i], NULL) == 0);
  }
  printf("SUCCEEDED (%d heap sections added, heap size: %lu KiB,"
         " virtual dirty bits: 0x%x, parallel: %d)\n",
         N_GROWERS * N_SECTIONS, (unsigned long)(GC_get_heap_size() >> 10),
         GC_get_actual_vdb(), GC_get_parallel());
  return 0;
}

#else

int
main(void)
{
#  if !defined(PARALLEL_MARK) || !defined(THREAD_LOCAL_ALLOC)
  printf("test skipped (no parallel marking or thread-local allocation)\n");
#  else
  printf("test skipped\n");
#  endif
  return 0;
}

#endif
