/*
 * Test "no thread-local warm-up" mode: whether a thread allocates its
 * first small objects of a size from the global free lists (one object
 * at a time) or from its thread-local free list (taken at once).
 * The objects are allocated by the non-debug functions even if `GC_DEBUG`
 * is defined, as a debug object is bigger than requested.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef GC_THREADS
#  define GC_THREADS
#endif

#include "gc.h"
#include "gc/gc_tiny_fl.h"

#ifdef GC_PTHREADS
#  include <pthread.h>
#else
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  define NOSERVICE
#  include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/*
 * Only the sizes of up to `GC_TINY_FREELISTS - 1` granules are allocated
 * from the thread-local free lists.  Each check uses a size (in granules)
 * of its own, starting from the biggest one.
 */
#define N_CHECKS 3
#if defined(THREAD_LOCAL_ALLOC) && GC_TINY_FREELISTS > N_CHECKS + 1
#  define CHECK_ALLOCATIONS
#endif

/*
 * The amount of the heap expansion in advance of the checks, so that each
 * thread-local free list is refilled with a whole block of objects (if no
 * free block is available, `GC_generic_malloc_many()` grows the heap and
 * allocates just one object).  Each check refills up to two free lists
 * (one per kind), each of up to the biggest supported heap block (64 KB).
 */
#define HEAP_RESERVE_BYTES (N_CHECKS * 2 * 64 * 1024)

#ifdef CHECK_ALLOCATIONS
static void *
checked(void *p)
{
  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}
#endif

/*
 * Allocate two objects of `lg` granules of either kind, and check whether
 * the first ones are allocated from the thread-local free list: taking
 * the free list counts all of its objects as allocated at once (and the
 * next allocations from it are not counted), while a global allocation
 * counts the allocated object.  The collections should be disabled.
 * Skipped in the manual VDB mode, as a thread-local free list is refilled
 * by a single object at a time in that case.
 */
static void
check_first_allocations(int lg, int expect_warmup)
{
#ifdef CHECK_ALLOCATIONS
  size_t lb = (size_t)(lg - 1) * GC_GRANULE_BYTES + 1;
  int atomic;

  if (GC_VDB_MANUAL == GC_get_actual_vdb())
    return;
  for (atomic = 0; atomic <= 1; atomic++) {
    size_t bytes0 = GC_get_total_bytes();
    const void *p = checked(atomic ? GC_malloc_atomic(lb) : GC_malloc(lb));
    size_t bytes1 = GC_get_total_bytes();
    const void *q = checked(atomic ? GC_malloc_atomic(lb) : GC_malloc(lb));
    size_t bytes2 = GC_get_total_bytes();

    if (expect_warmup) {
      CHECK(bytes1 - bytes0 == GC_size(p));
      CHECK(bytes2 - bytes1 == GC_size(q));
    } else {
      CHECK(bytes1 - bytes0 > GC_size(p));
      CHECK(bytes2 == bytes1);
    }
  }
#else
  (void)lg;
  (void)expect_warmup;
#endif
}

struct check_s {
  int lg;
  int expect_warmup;
};

#ifdef GC_PTHREADS
static void *
thread_check(void *arg)
#else
static DWORD WINAPI
thread_check(LPVOID arg)
#endif
{
  const struct check_s *pcheck = (const struct check_s *)arg;

  check_first_allocations(pcheck->lg, pcheck->expect_warmup);
#ifdef GC_PTHREADS
  return arg;
#else
  return (DWORD)(GC_uintptr_t)arg;
#endif
}

/* Run `check_first_allocations(lg, !no_warmup)` in a new thread. */
static void
check_new_thread(int lg, int no_warmup)
{
  struct check_s check;
#ifdef GC_PTHREADS
  pthread_t t;
  int err;
#else
  HANDLE t;
  DWORD thread_id;
#endif

  GC_set_no_thread_local_warmup(no_warmup);
  CHECK(GC_get_no_thread_local_warmup() == no_warmup);
  check.lg = lg;
  check.expect_warmup = !no_warmup;
#ifdef GC_PTHREADS
  err = pthread_create(&t, NULL, thread_check, &check);
  if (err != 0) {
    fprintf(stderr, "Thread creation failed, errno= %d\n", err);
    exit(69);
  }
  CHECK(pthread_join(t, NULL) == 0);
#else
  t = CreateThread(NULL, 0, thread_check, &check, 0, &thread_id);
  if (NULL == t) {
    fprintf(stderr, "Thread creation failed, errcode= %d\n",
            (int)GetLastError());
    exit(69);
  }
  CHECK(WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0);
  CloseHandle(t);
#endif
}

int
main(void)
{
  const char *str;
  int no_warmup;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  /* This is optional if `pthread_create()` is redirected. */
  GC_allow_register_threads();

  /* Check the initial mode. */
  str = getenv("GC_NO_THREAD_LOCAL_WARMUP");
  if (str != NULL) {
    no_warmup = !(str[0] == '0' && str[1] == '\0');
  } else {
#ifdef GC_NO_THREAD_LOCAL_WARMUP
    no_warmup = 1;
#else
    no_warmup = 0;
#endif
  }
  CHECK(GC_get_no_thread_local_warmup() == no_warmup);
  printf("Initial no-thread-local-warm-up mode: %d\n", no_warmup);

#ifdef CHECK_ALLOCATIONS
  if (!GC_expand_hp(HEAP_RESERVE_BYTES)) {
    fprintf(stderr, "Heap expansion failed\n");
    exit(69);
  }
#endif
  /* The collections would change the counted bytes. */
  GC_disable();
  /* The main thread is registered in the initial mode. */
  check_first_allocations(GC_TINY_FREELISTS - 1, !no_warmup);
  /* A new thread is registered in the mode set at its creation. */
  check_new_thread(GC_TINY_FREELISTS - 2, !no_warmup);
  check_new_thread(GC_TINY_FREELISTS - 3, no_warmup);
  GC_set_no_thread_local_warmup(no_warmup);
  GC_enable();
  printf("SUCCEEDED\n");
  return 0;
}
