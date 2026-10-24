/*
 * Test that the collector collects while a thread created by
 * `GC_pthread_create()` is exiting, i.e. after the thread has called
 * `pthread_exit()` or has acted on a cancellation request (of its own or
 * of the main thread) and before the collector unregisters it in the
 * thread exit handler (a cleanup handler).  The exiting thread runs
 * a cleanup handler of its own, which is called before the one of the
 * collector, and which waits there until the main thread has done its
 * checks.  The test fails if the collection is disabled meanwhile, if
 * `GC_gcollect()` does not complete a collection, if the collection does
 * not stop the exiting thread (as any other running thread), if an object
 * referenced only from the stack of the exiting thread is reclaimed (a
 * disappearing link to it is cleared, or its content is changed), or if
 * the heap grows by half of the size of the unreachable objects which the
 * main thread allocates meanwhile (32 MiB of 64 MiB by default).  Where
 * the collector intercepts `pthread_exit()` and `pthread_cancel()`, the
 * thread also exits from inside `GC_do_blocking()`; then the collection
 * may be disabled, but a collection must still stop the thread, and must
 * not use the stack frames unwound by the exit (the cleanup handler
 * overwrites them).
 * A thread-specific data destructor of the exiting thread registers the
 * thread again and unregisters it, then the main thread makes
 * a cancellation request for the thread; the collection must not be
 * disabled after the thread is joined.  On Linux, where
 * `GC_suspend_thread()` stops a thread outside `dl_iterate_phdr()`, three
 * more cases exit as the first three ones, but the cleanup handler of the
 * exiting thread first waits inside a callback of `dl_iterate_phdr()` (as
 * the unwinder of the exit might do) until the main thread starts to
 * suspend the thread by `GC_suspend_thread()`, and 100 ms more.  The main
 * thread then checks that the thread makes no progress while suspended,
 * calls `GC_gcollect()` and resumes the thread.  The test fails if this
 * does not complete within a time limit (60 s by default).
 * A thread which a collection stops is counted once, even if the
 * collector sends the suspend signal to it again.  In the leak detection
 * mode, the disappearing link and the heap growth are not checked, and
 * without finalization (`GC_NO_FINALIZATION`), the disappearing link is
 * not checked.  The test runs on Linux, where the collector intercepts
 * `pthread_exit()` and `pthread_cancel()`, and on Darwin, where it does
 * not.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
/* For `dl_iterate_phdr()`. */
#  define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
/* For `GC_THREADS` macro. */
#  include "config.h"
#endif

#define GC_NO_THREAD_REDIRECTS 1
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(GC_PTHREADS) && (defined(LINUX) || defined(DARWIN)) \
    && defined(AO_HAVE_load) && defined(AO_HAVE_store)          \
    && defined(AO_HAVE_load_acquire) && defined(AO_HAVE_store_release)

#  include <errno.h>
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>

#  if defined(LINUX) && defined(CAN_CALL_IN_DL_ITERATE_PHDR)
#    include "gc/javaxfc.h"
#    include <link.h>
#    include <signal.h>
#    include <unistd.h>

/*
 * The main thread suspends the exiting thread while the latter waits
 * inside a callback of `dl_iterate_phdr()`.  `GC_suspend_thread()` stops
 * the thread outside `dl_iterate_phdr()` only if this macro is defined
 * (not with ThreadSanitizer, for example).
 */
#    define TEST_SUSPEND_IN_DL_ITERATE_PHDR
#  endif

#  define TEST_ASSERT(e)                                                    \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }

#  ifdef GC_NO_PTHREAD_CANCEL
#    define CANCEL_THREAD(t) pthread_cancel(t)
#  else
#    define CANCEL_THREAD(t) GC_pthread_cancel(t)
#  endif

#  ifdef GC_HAVE_PTHREAD_EXIT
#    define EXIT_THREAD(v) GC_pthread_exit(v)
#  else
#    define EXIT_THREAD(v) pthread_exit(v)
#  endif

/*
 * The size of the unreachable objects which the main thread allocates
 * while the other thread is exiting.
 */
#  ifndef GARBAGE_BYTES
#    define GARBAGE_BYTES (64 * 1024 * 1024)
#  endif

#  define GARBAGE_OBJ_BYTES 1024

/* The number of the runs of each case. */
#  ifndef N_ROUNDS
#    define N_ROUNDS 3
#  endif

/*
 * The time limit (in seconds) of the suspension of the exiting thread,
 * of the collection and of the resumption.
 */
#  ifndef SUSPEND_TIMEOUT
#    define SUSPEND_TIMEOUT 60
#  endif

/*
 * How long (in milliseconds) the exiting thread stays inside the callback
 * of `dl_iterate_phdr()` after the main thread starts to suspend it.
 */
#  ifndef SUSPEND_WAIT_MS
#    define SUSPEND_WAIT_MS 100
#  endif

/* The value stored in the object referenced from the exiting thread. */
#  define OBJ_MAGIC ((GC_word)0x5a5a1234)

/* How the thread exits. */
enum exit_kind { BY_EXIT, BY_CANCEL, BY_SELF_CANCEL };

/* Where the thread exits. */
enum exit_place {
  ACTIVE,
  /* Inside `GC_do_blocking()`. */
  IN_BLOCKING,
  /* Inside `GC_call_with_gc_active()` inside `GC_do_blocking()`. */
  IN_ACTIVE_IN_BLOCKING
};

struct exit_case {
  const char *name;
  enum exit_kind kind;
  enum exit_place place;
  /*
   * Nonzero if the main thread suspends the exiting thread while the
   * latter waits inside a callback of `dl_iterate_phdr()`.
   */
  int suspend;
};

static struct exit_case exit_cases[] = {
  { "pthread_exit", BY_EXIT, ACTIVE, 0 },
  { "pthread_cancel", BY_CANCEL, ACTIVE, 0 },
  { "pthread_cancel of self", BY_SELF_CANCEL, ACTIVE, 0 },
#  ifdef GC_HAVE_PTHREAD_EXIT
  { "pthread_exit in GC_call_with_gc_active in GC_do_blocking", BY_EXIT,
    IN_ACTIVE_IN_BLOCKING, 0 },
#  endif
#  if !defined(GC_NO_PTHREAD_CANCEL) && defined(CANCEL_SAFE)
  /* The request is made while the thread is inside `GC_do_blocking()`. */
  { "pthread_cancel in GC_do_blocking", BY_CANCEL, IN_BLOCKING, 0 },
  /* The request is made before the thread enters `GC_do_blocking()`. */
  { "pthread_cancel of self before GC_do_blocking", BY_SELF_CANCEL,
    IN_BLOCKING, 0 },
#  endif
#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
#    ifdef GC_HAVE_PTHREAD_EXIT
  { "pthread_exit, suspended in dl_iterate_phdr", BY_EXIT, ACTIVE, 1 },
#    endif
#    if !defined(GC_NO_PTHREAD_CANCEL) && defined(CANCEL_SAFE)
  { "pthread_cancel, suspended in dl_iterate_phdr", BY_CANCEL, ACTIVE, 1 },
  { "pthread_cancel of self, suspended in dl_iterate_phdr", BY_SELF_CANCEL,
    ACTIVE, 1 },
#    endif
#  endif
};

/* Set by the cleanup handler of the exiting thread. */
static volatile AO_t in_window;

/* Set by the main thread once it has done the checks. */
static volatile AO_t window_checked;

/* Set by the exiting thread before it waits to be canceled. */
static volatile AO_t ready_to_cancel;

/* Set by the cleanup handler if the object is intact after the checks. */
static volatile AO_t obj_intact;

/*
 * The hidden pointer to the object referenced from the exiting thread,
 * cleared by the collector if the object is reclaimed.
 */
static GC_hidden_pointer obj_link;

/* The exit status of the threads which call `pthread_exit()`. */
static int exit_status;

/*
 * Nonzero in the leak detection mode, where unreachable objects are not
 * reclaimed and disappearing links are not supported.
 */
static int find_leak;

#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
/* The case run by the exiting thread. */
static struct exit_case *current_case;

/* Set by the exiting thread inside the callback of `dl_iterate_phdr()`. */
static volatile AO_t in_dl_iterate_phdr;

/* Set by the main thread just before it suspends the exiting thread. */
static volatile AO_t suspending;

/*
 * Incremented by the exiting thread in its loops of `sched_yield()` (in
 * the callback and in the cleanup handler).
 */
static volatile AO_t exiting_progress;
#  endif

/*
 * The threads suspended by the collector, each counted once.  The
 * collector sends the suspend signal again to a thread which has not
 * acknowledged it in time (or whose signal is lost on purpose with
 * `SIMULATE_LOST_SIGNALS`), and reports each signal it sends.  Accessed
 * with the allocator lock held, not by atomic operations: these may be
 * emulated with a lock, which a stopped thread may hold.
 */
#  define MAX_SUSPENDED 16
static void *suspended_ids[MAX_SUSPENDED];
static unsigned suspended_count;

/* The key of the thread-specific data of the exiting thread. */
static pthread_key_t register_again_key;

/*
 * Set by the thread-specific data destructor once it has unregistered the
 * exiting thread again.
 */
static volatile AO_t unregistered;

static void GC_CALLBACK
on_thread_event(GC_EventType event, void *thread_id)
{
  unsigned i;

  /* Called with the allocator lock held. */
  if (event != GC_EVENT_THREAD_SUSPENDED)
    return;
  for (i = 0; i < suspended_count; i++) {
    if (suspended_ids[i] == thread_id)
      return;
  }
  /* A collection here stops at most two threads. */
  if (suspended_count < MAX_SUSPENDED)
    suspended_ids[suspended_count++] = thread_id;
}

static void *GC_CALLBACK
reset_suspended_count(void *arg)
{
  (void)arg;
  suspended_count = 0;
  return NULL;
}

static void *GC_CALLBACK
get_suspended_count(void *pcount)
{
  *(unsigned *)pcount = suspended_count;
  return NULL;
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
  unsigned count;

  (void)GC_call_with_alloc_lock(reset_suspended_count, NULL);
  GC_gcollect();
  (void)GC_call_with_alloc_lock(get_suspended_count, &count);
  return count;
}

static size_t
heap_size(void)
{
  return GC_get_heap_size() + GC_get_unmapped_bytes();
}

static void *GC_CALLBACK
register_again_inner(struct GC_stack_base *sb, void *arg)
{
  (void)arg;
  TEST_ASSERT(GC_register_my_thread(sb) == GC_SUCCESS);
  TEST_ASSERT(GC_unregister_my_thread() == GC_SUCCESS);
  AO_store_release(&unregistered, 1);
  return NULL;
}

/*
 * The destructor of the thread-specific data of the exiting thread.  It is
 * called after the collector unregisters the thread.
 */
static void
register_again(void *value)
{
  (void)value;
  (void)GC_call_with_stack_base(register_again_inner, NULL);
}

/*
 * Overwrite the part of the stack below the frame of the caller, where
 * the frames unwound by the thread exit were, as deeper calls would do.
 */
static GC_ATTR_NOINLINE void
overwrite_stack(void)
{
  volatile unsigned char buf[32 * 1024];
  size_t i;

  for (i = 0; i < sizeof(buf); i++)
    buf[i] = 0xa5;
}

#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
static long
elapsed_ms(const struct timespec *since)
{
  struct timespec now;

  TEST_ASSERT(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (long)(now.tv_sec - since->tv_sec) * 1000
         + (now.tv_nsec - since->tv_nsec) / 1000000;
}

/*
 * The callback of `dl_iterate_phdr()`.  `glibc` holds a lock meanwhile,
 * which a collection acquires before it stops the world.  Wait until the
 * main thread starts to suspend this thread, and `SUSPEND_WAIT_MS` more.
 */
static int
wait_in_dl_iterate_phdr(struct dl_phdr_info *info, size_t size, void *data)
{
  struct timespec start;

  (void)info;
  (void)size;
  (void)data;
  AO_store_release(&in_dl_iterate_phdr, 1);
  while (!AO_load_acquire(&suspending))
    sched_yield();
  TEST_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  while (elapsed_ms(&start) < SUSPEND_WAIT_MS) {
    AO_store(&exiting_progress, AO_load(&exiting_progress) + 1);
    sched_yield();
  }
  /* Stop the iteration. */
  return 1;
}

static void
on_alarm(int sig)
{
  static const char msg[] = "Collection deadlocked while the exiting thread"
                            " is suspended in dl_iterate_phdr\n";

  (void)sig;
  if (write(2, msg, sizeof(msg) - 1) < 0) {
    /* Ignore the error, exit anyway. */
  }
  _exit(1);
}

/*
 * Suspend the exiting thread while it waits inside the callback of
 * `dl_iterate_phdr()`, check that it makes no progress while suspended,
 * collect, and resume the thread.  Exit with failure if this does not
 * complete within `SUSPEND_TIMEOUT` seconds.
 */
static void
collect_while_suspended(pthread_t t)
{
  AO_t progress;

  while (!AO_load_acquire(&in_dl_iterate_phdr))
    sleep_ms(1);
  TEST_ASSERT(signal(SIGALRM, on_alarm) != SIG_ERR);
  (void)alarm(SUSPEND_TIMEOUT);
  AO_store_release(&suspending, 1);
  GC_suspend_thread(t);
  TEST_ASSERT(GC_is_thread_suspended(t));
  /*
   * The thread is suspended, maybe inside an atomic operation on the
   * counter, which holds a lock if the operations are emulated, thus the
   * counter is read by plain loads until the thread is resumed.
   */
  progress = exiting_progress;
  sleep_ms(10);
  GC_gcollect();
  TEST_ASSERT(exiting_progress == progress);
  GC_resume_thread(t);
  (void)alarm(0);
}
#  endif

/*
 * The cleanup handler of the exiting thread.  It is called before the
 * thread exit handler of the collector.  The object is referenced only
 * from the stack of this thread.
 */
static void
wait_in_window(void *arg)
{
  GC_word *volatile obj = (GC_word *)arg;

  overwrite_stack();
#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
  if (current_case->suspend)
    (void)dl_iterate_phdr(wait_in_dl_iterate_phdr, NULL);
#  endif
  AO_store_release(&in_window, 1);
  while (!AO_load_acquire(&window_checked)) {
#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
    AO_store(&exiting_progress, AO_load(&exiting_progress) + 1);
#  endif
    sched_yield();
  }
  if (OBJ_MAGIC == obj[1])
    AO_store(&obj_intact, 1);
}

/*
 * Exit the thread, or wait until it is canceled.  Called inside
 * `GC_do_blocking()` for a case other than `ACTIVE` one.
 */
static void *GC_CALLBACK
exit_or_wait(void *arg)
{
  struct exit_case *c = (struct exit_case *)arg;

  if (BY_EXIT == c->kind)
    EXIT_THREAD(&exit_status);
  if (BY_CANCEL == c->kind)
    AO_store(&ready_to_cancel, 1);
  /*
   * Wait until canceled.  The thread acts on the request only in
   * `pthread_testcancel()`: ThreadSanitizer reports false data races
   * (on accesses under the allocator lock) after a thread acts on
   * a cancellation request inside an intercepted function like
   * `nanosleep()`.
   */
  while (!AO_load(&in_window)) {
    int state;

    pthread_testcancel();
    TEST_ASSERT(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state) == 0);
    sleep_ms(1);
    TEST_ASSERT(pthread_setcancelstate(state, &state) == 0);
  }
  return NULL;
}

static void *GC_CALLBACK
blocking_fn(void *arg)
{
  struct exit_case *c = (struct exit_case *)arg;

  if (IN_ACTIVE_IN_BLOCKING == c->place)
    return GC_call_with_gc_active(exit_or_wait, arg);
  return exit_or_wait(arg);
}

static void *
exiting_thread(void *arg)
{
  struct exit_case *c = (struct exit_case *)arg;
  GC_word *obj = (GC_word *)GC_MALLOC(4 * sizeof(GC_word));

  TEST_ASSERT(obj != NULL);
  obj[1] = OBJ_MAGIC;
  obj_link = GC_HIDE_POINTER(obj);
#  ifndef GC_NO_FINALIZATION
  if (!find_leak)
    TEST_ASSERT(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&obj_link, obj)
                == GC_SUCCESS);
#  endif
  TEST_ASSERT(pthread_setspecific(register_again_key, &exit_status) == 0);
  pthread_cleanup_push(wait_in_window, obj);
  if (BY_SELF_CANCEL == c->kind)
    TEST_ASSERT(CANCEL_THREAD(pthread_self()) == 0);
  if (ACTIVE == c->place) {
    (void)exit_or_wait(arg);
  } else {
    (void)GC_do_blocking(blocking_fn, arg);
  }
  pthread_cleanup_pop(0);
  return NULL;
}

/*
 * Run a thread which exits in the given way, and check the collection
 * while the thread is exiting.  Exit with failure if a check fails.
 */
static void
run_exiting_thread(struct exit_case *c, unsigned stopped_before)
{
  pthread_t t;
  void *status;
  int disabled, collected, link_cleared, failed;
  unsigned stopped;
  GC_word gc_no;
  size_t heap_before, heap_growth;
  long i;

  AO_store(&in_window, 0);
  AO_store(&window_checked, 0);
  AO_store(&ready_to_cancel, 0);
  AO_store(&obj_intact, 0);
  AO_store(&unregistered, 0);
#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
  AO_store(&in_dl_iterate_phdr, 0);
  AO_store(&suspending, 0);
  AO_store(&exiting_progress, 0);
  current_case = c;
#  endif
  TEST_ASSERT(GC_pthread_create(&t, NULL, exiting_thread, (void *)c) == 0);
  if (BY_CANCEL == c->kind) {
    while (!AO_load(&ready_to_cancel))
      sleep_ms(1);
    TEST_ASSERT(CANCEL_THREAD(t) == 0);
  }
#  ifdef TEST_SUSPEND_IN_DL_ITERATE_PHDR
  if (c->suspend)
    collect_while_suspended(t);
#  endif
  while (!AO_load_acquire(&in_window))
    sleep_ms(1);

  /*
   * The other thread is exiting and waits in its cleanup handler.
   * The collection may be disabled only if the thread exits from inside
   * `GC_do_blocking()`, but a collection must stop the thread anyway.
   */
  disabled = GC_is_disabled();
  gc_no = GC_get_gc_no();
  stopped = count_stopped_threads();
  collected = GC_get_gc_no() != gc_no;
  heap_before = heap_size();
  if (!disabled && !find_leak) {
    for (i = 0; i < GARBAGE_BYTES / GARBAGE_OBJ_BYTES; i++) {
      TEST_ASSERT(GC_MALLOC(GARBAGE_OBJ_BYTES) != NULL);
    }
  }
  heap_growth = heap_size() - heap_before;
  link_cleared = 0 == obj_link;
  AO_store_release(&window_checked, 1);

  /*
   * A cancellation request for the thread after it is unregistered must
   * not disable the collection.
   */
  while (!AO_load_acquire(&unregistered))
    sleep_ms(1);
  (void)CANCEL_THREAD(t);
  TEST_ASSERT(GC_pthread_join(t, &status) == 0);
  failed = 0;
  if (disabled && ACTIVE == c->place) {
    fprintf(stderr, "%s: collection disabled while thread exits\n", c->name);
    failed = 1;
  }
  if (!disabled && !collected) {
    fprintf(stderr, "%s: no collection while thread exits\n", c->name);
    failed = 1;
  } else if (collected && stopped != stopped_before + 1) {
    fprintf(stderr, "%s: collection stopped %u threads, expected %u\n",
            c->name, stopped, stopped_before + 1);
    failed = 1;
  }
  if (link_cleared || !AO_load(&obj_intact)) {
    fprintf(stderr, "%s: object of exiting thread reclaimed\n", c->name);
    failed = 1;
  }
  if (heap_growth >= (size_t)GARBAGE_BYTES / 2) {
    fprintf(stderr,
            "%s: heap grown by %lu KiB while thread exits"
            " (allocated %lu KiB)\n",
            c->name, (unsigned long)(heap_growth >> 10),
            (unsigned long)(GARBAGE_BYTES >> 10));
    failed = 1;
  }
  if (GC_is_disabled()) {
    fprintf(stderr, "%s: collection disabled after thread is joined\n",
            c->name);
    failed = 1;
  }
  if (failed)
    exit(1);
  TEST_ASSERT(
      status
      == (BY_EXIT == c->kind ? (void *)&exit_status : PTHREAD_CANCELED));
#  ifndef GC_NO_FINALIZATION
  (void)GC_unregister_disappearing_link((void **)&obj_link);
#  endif
  TEST_ASSERT(count_stopped_threads() == stopped_before);
}

int
main(void)
{
  unsigned stopped_before;
  size_t j;
  int i;

  GC_INIT();
  find_leak = GC_get_find_leak();
  if (find_leak)
    printf("This test program is not designed for leak detection mode\n");
  TEST_ASSERT(pthread_key_create(&register_again_key, register_again) == 0);
  GC_set_on_thread_event(on_thread_event);
  stopped_before = count_stopped_threads();
  for (i = 0; i < N_ROUNDS; i++) {
    for (j = 0; j < sizeof(exit_cases) / sizeof(exit_cases[0]); j++) {
      run_exiting_thread(&exit_cases[j], stopped_before);
    }
  }
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
