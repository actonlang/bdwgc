/*
 * Copyright (c) 2026 Acton contributors
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

/*
 * Test the collection while a thread exists that has not run yet.  The
 * main thread creates a detached thread by `pthread_create_suspended_np()`,
 * which stays suspended at its first instruction, with the zero frame
 * pointer, until the main thread resumes it at the end.  The argument of
 * its start routine is a new object, which nothing else references.  Then
 * the main thread creates a thread by `GC_pthread_create()`, which collects
 * 3 times.  The system places the stack of this collecting thread above the
 * stack of the thread that has not run (the test prints whether it does).
 * With `DARWIN_PARSE_STACK` the collector scans every thread of the
 * process, including the one that has not run.  The stack range of that
 * thread must not reach up to the stack of the collecting thread: such
 * a range covers the guard page below the latter, and the marking reads
 * the guard page, thus the process gets `SIGBUS`, or hangs if a marker
 * thread (which blocks all signals) does the read.  The object must
 * survive the collections, as the argument is in a register of the thread
 * that has not run.  Without `DARWIN_PARSE_STACK` the collector scans
 * only the registered threads, thus the object is not checked.
 * The thread that has not run is not joined: the collector does not know
 * it, and with `GC_USE_DLOPEN_WRAP` (defined if `malloc` is redirected)
 * `pthread_join()` is the collector's wrapper, which needs a thread that
 * the collector knows.
 * The test fails if it does not complete within a time limit (60 s by
 * default) and, with `DARWIN_PARSE_STACK`, if the disappearing link to the
 * object is cleared or if the contents of the object have changed.  In the
 * leak detection mode, and without finalization (`GC_NO_FINALIZATION`),
 * the disappearing link is not checked.  The test runs on Darwin only.
 */

#ifdef HAVE_CONFIG_H
/* For `GC_THREADS` macro. */
#  include "config.h"
#endif

#define GC_NO_THREAD_REDIRECTS 1
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(GC_PTHREADS) && defined(DARWIN)

#  include <mach/mach.h>
#  include <pthread.h>
#  include <signal.h>
#  include <string.h>
#  include <unistd.h>

#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

/* The time limit (in seconds) of the test. */
#  ifndef TEST_TIMEOUT
#    define TEST_TIMEOUT 60
#  endif

/* The collections done by the collecting thread. */
#  define N_COLLECTIONS 3

#  define OBJ_MAGIC ((GC_word)0x5a5a1234)

/*
 * The hidden pointer to the object passed to the thread that has not
 * run, cleared by the collector if the object is reclaimed.
 */
static GC_hidden_pointer obj_link;

/*
 * Nonzero in the leak detection mode, where unreachable objects are not
 * reclaimed and disappearing links are not supported.
 */
static int find_leak;

/* The stack of the thread that has not run. */
static char *stack_lo;
static char *stack_hi;

static void *
return_arg(void *arg)
{
  return arg;
}

/*
 * Create the thread that does not run until it is resumed, with a new
 * object as the argument of its start routine.  Not inlined, so that the
 * pointer to the object is only in frames below the one of the caller,
 * which `clear_stack()` overwrites.
 */
static GC_ATTR_NOINLINE pthread_t
create_suspended_thread(void)
{
  pthread_t t;
  pthread_attr_t attr;
  GC_word *obj = (GC_word *)GC_MALLOC(4 * sizeof(GC_word));

  CHECK(obj != NULL);
  obj[1] = OBJ_MAGIC;
  obj_link = GC_HIDE_POINTER(obj);
#  ifndef GC_NO_FINALIZATION
  if (!find_leak)
    CHECK(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&obj_link, obj)
          == GC_SUCCESS);
#  endif
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
  CHECK(pthread_create_suspended_np(&t, &attr, return_arg, obj) == 0);
  (void)pthread_attr_destroy(&attr);
  return t;
}

/* Overwrite the stack below the caller, to drop stale pointers. */
static GC_ATTR_NOINLINE void
clear_stack(void)
{
  volatile char buf[32 * 1024];

  memset((char *)buf, 0, sizeof(buf));
  GC_noop1((GC_word)(GC_uintptr_t)buf);
}

static void *
collect(void *arg)
{
  pthread_t self = pthread_self();
  char *hi = (char *)pthread_get_stackaddr_np(self);
  char *lo = hi - pthread_get_stacksize_np(self);
  int i;

  (void)arg;
  printf("Stack of the thread that has not run [%p, %p),"
         " of the collecting thread [%p, %p), %s\n",
         (void *)stack_lo, (void *)stack_hi, (void *)lo, (void *)hi,
         (GC_uintptr_t)lo >= (GC_uintptr_t)stack_hi ? "above" : "not above");
  fflush(stdout);
  for (i = 0; i < N_COLLECTIONS; i++)
    GC_gcollect();
  return NULL;
}

int
main(void)
{
  pthread_t t, collecting;

  GC_INIT();
  find_leak = GC_get_find_leak();
  /*
   * The time limit turns a hang into a failure.  `SIGALRM` is not caught,
   * as the system might choose a thread suspended by the collector to run
   * the handler.
   */
  CHECK(signal(SIGALRM, SIG_DFL) != SIG_ERR);
  (void)alarm(TEST_TIMEOUT);

  t = create_suspended_thread();
  stack_hi = (char *)pthread_get_stackaddr_np(t);
  stack_lo = stack_hi - pthread_get_stacksize_np(t);
  clear_stack();
  CHECK(GC_pthread_create(&collecting, NULL, collect, NULL) == 0);
  CHECK(GC_pthread_join(collecting, NULL) == 0);
#  ifdef DARWIN_PARSE_STACK
#    ifndef GC_NO_FINALIZATION
  if (!find_leak && 0 == obj_link) {
    fprintf(stderr, "Object of the thread that has not run reclaimed\n");
    exit(1);
  }
#    endif
  CHECK(((GC_word *)GC_REVEAL_POINTER(obj_link))[1] == OBJ_MAGIC);
#  endif
#  ifndef GC_NO_FINALIZATION
  if (!find_leak)
    (void)GC_unregister_disappearing_link((void **)&obj_link);
#  endif

  /* The thread is detached, it terminates on its own. */
  CHECK(thread_resume(pthread_mach_thread_np(t)) == KERN_SUCCESS);
  (void)alarm(0);
  printf("SUCCEEDED\n");
  return 0;
}

#else

int
main(void)
{
  printf("test skipped\n");
  return 0;
}

#endif
