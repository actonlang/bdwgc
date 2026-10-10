/*
 * Test that a pending cancellation request does not stop the collector
 * from unregistering a thread, neither in the thread exit handler (when
 * the thread start routine returns) nor in `GC_unregister_my_thread()`.
 * A thread requests its own cancellation (deferred, thus it remains
 * pending until a cancellation point) and then returns (or unregisters
 * itself) while the main thread holds the allocator lock, so that the
 * collector waits for the lock in the thread being unregistered (the spin
 * lock sleeps in `nanosleep()`, a cancellation point, after a few
 * `sched_yield()` calls).  The test fails if the thread terminates while
 * the main thread still holds the lock (i.e. before it is unregistered),
 * if the collection remains disabled after the thread is joined (the
 * intercepted `pthread_cancel()` disables the collection until the target
 * thread is unregistered), if a collection stops a different number of
 * threads than before the thread was created, or if the cancellation
 * request is no longer pending when `GC_unregister_my_thread()` returns.
 * A thread which is never unregistered might also crash the collection
 * (the collector fails to suspend it), thus no collection is done after
 * such a failure.  The case of `GC_unregister_my_thread()` is not run if
 * `pthread_create()` called by the client can be the collector's wrapper
 * (`GC_USE_DLOPEN_WRAP`, defined if `malloc` is redirected, or
 * `GC_USE_LD_WRAP`).
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

#if defined(GC_PTHREADS) && defined(CANCEL_SAFE)         \
    && !defined(PLATFORM_THREADS) && !defined(NACL)      \
    && !defined(SN_TARGET_PSP2) && defined(AO_HAVE_load) \
    && defined(AO_HAVE_store)

#  include <errno.h>
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>

#  define TEST_ASSERT(e)                                                    \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }

/*
 * Milliseconds the main thread holds the allocator lock after the other
 * thread has started to return (or to unregister itself), unless the
 * other thread terminates earlier.
 */
#  ifndef HOLD_LOCK_MS
#    define HOLD_LOCK_MS 500
#  endif

#  ifdef GC_NO_PTHREAD_CANCEL
#    define CANCEL_THREAD(t) pthread_cancel(t)
#  else
#    define CANCEL_THREAD(t) GC_pthread_cancel(t)
#  endif

/*
 * With `GC_USE_DLOPEN_WRAP` or `GC_USE_LD_WRAP`, `pthread_create()` called
 * by the client can be the collector's wrapper, which registers the new
 * thread, thus the case of `GC_unregister_my_thread()` is not run.
 */
#  if !defined(GC_USE_DLOPEN_WRAP) && !defined(GC_USE_LD_WRAP)
#    define TEST_UNREGISTER_MY_THREAD
#  endif

/* The key whose destructor records that the current thread terminates. */
static pthread_key_t exit_key;

/* Set by the other thread once it has requested its own cancellation. */
static volatile AO_t cancel_requested;

/* Set by the main thread once it holds the allocator lock. */
static volatile AO_t lock_held;

/* Set by the destructor of `exit_key` when the other thread terminates. */
static volatile AO_t thread_exited;

/* The number of the threads suspended by the collector. */
static unsigned suspended_count;

static void
on_thread_exit(void *value)
{
  (void)value;
  AO_store(&thread_exited, 1);
}

static void GC_CALLBACK
on_thread_event(GC_EventType event, void *thread_id)
{
  (void)thread_id;
  if (GC_EVENT_THREAD_SUSPENDED == event)
    suspended_count++;
}

static void
sleep_ms(long ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) == -1 && EINTR == errno) {
    /* Continue sleeping for the remaining time. */
  }
}

/* Collect and return the number of the threads the collection stopped. */
static unsigned
count_stopped_threads(void)
{
  suspended_count = 0;
  GC_gcollect();
  return suspended_count;
}

/*
 * Request the cancellation of the current thread, then wait until the
 * main thread holds the allocator lock.  Neither `pthread_setspecific()`
 * nor `sched_yield()` is a cancellation point, thus the request remains
 * pending.
 */
static void
cancel_self_and_wait_for_lock(void)
{
  /* The destructor is called only for a non-`NULL` value. */
  TEST_ASSERT(pthread_setspecific(exit_key, &exit_key) == 0);
  TEST_ASSERT(CANCEL_THREAD(pthread_self()) == 0);
  AO_store(&cancel_requested, 1);
  while (!AO_load(&lock_held))
    sched_yield();
}

/* The thread created by `GC_pthread_create()`. */
static void *
return_while_canceled(void *arg)
{
  cancel_self_and_wait_for_lock();
  /* The thread exit handler is called by `pthread_cleanup_pop(1)`. */
  return arg;
}

#  ifdef TEST_UNREGISTER_MY_THREAD
/* The thread registered by `GC_register_my_thread()`. */
static void *
unregister_while_canceled(void *arg)
{
  struct GC_stack_base sb;

  TEST_ASSERT(GC_get_stack_base(&sb) == GC_SUCCESS);
  TEST_ASSERT(GC_register_my_thread(&sb) == GC_SUCCESS);
  cancel_self_and_wait_for_lock();
  TEST_ASSERT(GC_unregister_my_thread() == GC_SUCCESS);
  /* The cancellation request should still be pending. */
  pthread_testcancel();
  return arg;
}
#  endif

static void *GC_CALLBACK
hold_lock(void *arg)
{
  int i;

  (void)arg;
  AO_store(&lock_held, 1);
  for (i = 0; i < HOLD_LOCK_MS && !AO_load(&thread_exited); ++i)
    sleep_ms(1);
  return (void *)(GC_uintptr_t)AO_load(&thread_exited);
}

/*
 * Run the given start routine in a new thread (created by
 * `GC_pthread_create()` if `by_gc`), hold the allocator lock while the
 * thread returns or unregisters itself, and join the thread.  Exit with
 * failure if the thread is not unregistered properly, or if its exit
 * status differs from `expected_status` (unless the latter is `NULL`).
 */
static void
run_canceled_thread(const char *name, void *(*start_routine)(void *),
                    int by_gc, void *expected_status)
{
  pthread_t t;
  void *status;
  int err, exited_early, disabled;

  AO_store(&cancel_requested, 0);
  AO_store(&lock_held, 0);
  AO_store(&thread_exited, 0);
  err = by_gc ? GC_pthread_create(&t, NULL, start_routine, NULL)
              : pthread_create(&t, NULL, start_routine, NULL);
  if (err != 0) {
    fprintf(stderr, "Thread creation failed, errno= %d\n", err);
    exit(69);
  }
  while (!AO_load(&cancel_requested))
    sleep_ms(1);
  exited_early = (int)(GC_uintptr_t)GC_call_with_alloc_lock(hold_lock, NULL);
  TEST_ASSERT((by_gc ? GC_pthread_join(t, &status) : pthread_join(t, &status))
              == 0);
  disabled = GC_is_disabled();
  if (exited_early)
    fprintf(stderr, "%s: thread terminated while the lock was held\n", name);
  if (disabled)
    fprintf(stderr, "%s: collection disabled after thread join\n", name);
  if (exited_early || disabled)
    exit(1);
  if (expected_status != NULL)
    TEST_ASSERT(status == expected_status);
}

int
main(void)
{
  unsigned stopped_before;

  GC_INIT();
  GC_allow_register_threads();
  GC_set_on_thread_event(on_thread_event);
  TEST_ASSERT(pthread_key_create(&exit_key, on_thread_exit) == 0);
  stopped_before = count_stopped_threads();

  run_canceled_thread("thread exit handler", return_while_canceled, 1, NULL);
  TEST_ASSERT(count_stopped_threads() == stopped_before);
#  ifdef TEST_UNREGISTER_MY_THREAD
  run_canceled_thread("GC_unregister_my_thread", unregister_while_canceled, 0,
                      PTHREAD_CANCELED);
  TEST_ASSERT(count_stopped_threads() == stopped_before);
#  endif
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
