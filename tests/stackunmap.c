/*
 * Copyright (c) 2026 Kristian Larsson
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
 * A thread unregisters, and its stack becomes inaccessible, while a mark
 * of the incremental mode is in progress.  The collector must not read the
 * stack after that (if it does, the read faults).
 *
 * An unregistering thread waits for a collection in progress, releasing
 * the allocator lock between increments, until the collection number
 * changes.  Another thread can start a new collection meanwhile, whose
 * first world-stopped mark pushes the stack of the waiting thread and
 * ends at its time limit before the marking is done.
 *
 * In each round, the main thread starts a thread on a stack it maps
 * itself.  The thread registers itself, fills a large part of its stack
 * with pointers to objects only it holds, and waits.  The main thread
 * allocates until a world-stopped mark ends before the marking is done
 * (with a time limit of 1 ms unless the environment sets one), then
 * lets the thread unregister, from the frame which holds the pointers,
 * while it starts collections in a loop, yielding the processor between
 * them.  On Linux both threads run on one processor, so that the
 * processor yield of the waiting thread lets the main thread run.  The
 * main thread then joins the thread, makes its stack inaccessible, and
 * allocates until the mark completes.
 *
 * Without thread-local allocation, the allocator lock may be a spin lock,
 * which a thread waits for by sleeping up to 17 ms between tries.  The
 * main thread then yields the processor for 20 ms between the
 * collections, otherwise the unregistering thread rarely finds the lock
 * free, and the test may take minutes.  Not with ThreadSanitizer, where
 * the lock is a mutex: there a collection of the unregistering thread
 * then fails to stop the main thread.
 *
 * The test is skipped where `pthread_create()` called by the test can be
 * the collector's wrapper (`GC_USE_DLOPEN_WRAP`, defined if `malloc` is
 * redirected, or `GC_USE_LD_WRAP`), which registers the new thread, thus
 * the thread cannot register itself, and with `NO_CLOCK`, where
 * a world-stopped mark has no time limit.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
/* For `pthread_setaffinity_np()`. */
#  define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef GC_THREADS
#  define GC_THREADS
#endif
/* The thread registers and unregisters itself. */
#define GC_NO_THREAD_REDIRECTS 1

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(GC_PTHREADS) && !defined(GC_WIN32_THREADS)          \
    && !defined(__EMSCRIPTEN__) && !defined(GC_USE_DLOPEN_WRAP) \
    && !defined(GC_USE_LD_WRAP) && !defined(NO_CLOCK)
#  include <pthread.h>
#  include <sched.h>
#  include <sys/mman.h>
#  include <time.h>
#  include <unistd.h>

#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif

#  ifndef N_ROUNDS
#    define N_ROUNDS 20
#  endif

/*
 * The stack of the thread, and the part of it filled with pointers, which
 * takes longer to scan than the time limit.
 */
#  define STACK_BYTES ((size_t)16 * 1024 * 1024)
#  define FILL_WORDS ((size_t)1024 * 1024)

/* The objects the thread holds only on its stack. */
#  define N_OBJS 4096

/* The most bytes the main thread allocates waiting for an event. */
#  define MAX_WAIT_BYTES ((size_t)256 * 1024 * 1024)

#  if defined(__GNUC__) || defined(__clang__)
#    define NOINLINE __attribute__((__noinline__))
#  else
#    define NOINLINE /* empty */
#  endif

#  define CHECK(cond)                                                    \
    do {                                                                 \
      if (!(cond)) {                                                     \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                #cond);                                                  \
        exit(1);                                                         \
      }                                                                  \
    } while (0)

/* The flags of the two threads, accessed with `mutex` held while both run. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int thread_ready;
static int thread_go;
static int thread_unregistered;

/*
 * Set by the collection event handler, which runs in the collecting
 * thread holding the allocator lock, and read with the lock held too:
 * whether a world-stopped mark is in progress, and whether one ended
 * before the marking was done and the marking is still not done.
 */
static int in_stopped_mark;
static int mark_unfinished;

/*
 * Whether a mark was unfinished when the thread unregistered (read by
 * the main thread after it joins the thread).
 */
static int unfinished_at_unregister;

static void
on_collection_event(GC_EventType e)
{
  switch (e) {
  case GC_EVENT_MARK_START:
    in_stopped_mark = 1;
    break;
  case GC_EVENT_MARK_END:
    in_stopped_mark = 0;
    mark_unfinished = 0;
    break;
  case GC_EVENT_POST_START_WORLD:
    if (in_stopped_mark) {
      in_stopped_mark = 0;
      mark_unfinished = 1;
    }
    break;
  default:
    break;
  }
}

static void *GC_CALLBACK
read_mark_unfinished(void *pvalue)
{
  *(int *)pvalue = mark_unfinished;
  return NULL;
}

static int
is_mark_unfinished(void)
{
  int value;

  (void)GC_call_with_alloc_lock(read_mark_unfinished, &value);
  return value;
}

static void
wait_flag(const int *flag)
{
  CHECK(pthread_mutex_lock(&mutex) == 0);
  while (!*flag)
    CHECK(pthread_cond_wait(&cond, &mutex) == 0);
  CHECK(pthread_mutex_unlock(&mutex) == 0);
}

static int
get_flag(const int *flag)
{
  int value;

  CHECK(pthread_mutex_lock(&mutex) == 0);
  value = *flag;
  CHECK(pthread_mutex_unlock(&mutex) == 0);
  return value;
}

static void
set_flag(int *flag)
{
  CHECK(pthread_mutex_lock(&mutex) == 0);
  *flag = 1;
  CHECK(pthread_cond_broadcast(&cond) == 0);
  CHECK(pthread_mutex_unlock(&mutex) == 0);
}

#  if defined(__has_feature) && !defined(CPPCHECK)
#    if __has_feature(thread_sanitizer)
#      define TEST_THREAD_SANITIZER
#    endif
#  elif defined(__SANITIZE_THREAD__)
#    define TEST_THREAD_SANITIZER
#  endif

#  if !defined(THREAD_LOCAL_ALLOC) && !defined(TEST_THREAD_SANITIZER)
#    define YIELD_20MS

/* Yield the processor for 20 ms. */
static void
yield_20ms(void)
{
  struct timespec start, now;

  CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  do {
    sched_yield();
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  } while ((long)(now.tv_sec - start.tv_sec) * 1000
               + (now.tv_nsec - start.tv_nsec) / 1000000
           < 20);
}
#  endif

#  ifdef __linux__
/* Run the calling thread, and the threads it creates, on one processor. */
static void
pin_to_one_cpu(void)
{
  cpu_set_t set;
  int cpu;

  /* MemorySanitizer does not see the system call fill the set. */
  CPU_ZERO(&set);
  CHECK(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0);
  for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (CPU_ISSET(cpu, &set))
      break;
  }
  CHECK(cpu < CPU_SETSIZE);
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  CHECK(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0);
}
#  endif

static NOINLINE void
fill_and_unregister(void **objs)
{
  void *volatile fill[FILL_WORDS];
  size_t i;

  for (i = 0; i < FILL_WORDS; i++)
    fill[i] = objs[i % N_OBJS];
  set_flag(&thread_ready);
  wait_flag(&thread_go);
  CHECK(GC_unregister_my_thread() == GC_SUCCESS);
  unfinished_at_unregister = is_mark_unfinished();
  set_flag(&thread_unregistered);
  GC_noop1_ptr(fill[FILL_WORDS - 1]);
}

static void *
thread_proc(void *arg)
{
  struct GC_stack_base sb;
  void **objs = (void **)malloc(N_OBJS * sizeof(void *));
  size_t i;

  (void)arg;
  CHECK(objs != NULL);
  CHECK(GC_get_stack_base(&sb) == GC_SUCCESS);
  CHECK(GC_register_my_thread(&sb) == GC_SUCCESS);
  for (i = 0; i < N_OBJS; i++) {
    objs[i] = GC_MALLOC(4 * sizeof(void *));
    CHECK(objs[i] != NULL);
  }
  fill_and_unregister(objs);
  free(objs);
  return NULL;
}

/*
 * Allocate until `is_mark_unfinished()` returns `unfinished`; return
 * whether it did.
 */
static int
allocate_until(int unfinished)
{
  size_t bytes;
  void *keep = NULL;

  for (bytes = 0; bytes < MAX_WAIT_BYTES; bytes += 64) {
    void **p;

    if (is_mark_unfinished() == unfinished)
      break;
    p = (void **)GC_MALLOC(64);
    CHECK(p != NULL);
    if ((bytes & 0xfff) == 0) {
      /* A short list of live objects. */
      p[0] = keep;
      keep = (bytes & 0xffff) == 0 ? NULL : (void *)p;
    }
  }
  GC_noop1_ptr(keep);
  return bytes < MAX_WAIT_BYTES;
}

int
main(void)
{
  static void *stacks[N_ROUNDS];
  int round;
  int unfinished_rounds = 0;
  long page_size;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("Skipped: leak detection mode\n");
    return 0;
  }
  page_size = sysconf(_SC_PAGESIZE);
  CHECK(page_size > 0 && STACK_BYTES % (size_t)page_size == 0);
  GC_allow_register_threads();
  GC_set_on_collection_event(on_collection_event);
  GC_enable_incremental();
  if (getenv("GC_PAUSE_TIME_TARGET") == NULL) {
    struct GC_timeval_s tv;

    tv.tv_ms = 1;
    tv.tv_nsec = 0;
    GC_set_time_limit_tv(tv);
  }
#  ifdef __linux__
  /* After `GC_INIT()`, which starts the marker threads. */
  pin_to_one_cpu();
#  endif

  for (round = 0; round < N_ROUNDS; round++) {
    pthread_attr_t attr;
    pthread_t t;
    void *stack = mmap(NULL, STACK_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    CHECK(stack != MAP_FAILED);
    stacks[round] = stack;
    thread_ready = 0;
    thread_go = 0;
    thread_unregistered = 0;
    unfinished_at_unregister = 0;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstack(&attr, stack, STACK_BYTES) == 0);
    CHECK(pthread_create(&t, &attr, thread_proc, NULL) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    wait_flag(&thread_ready);

    if (GC_is_incremental_mode())
      (void)allocate_until(1);
    set_flag(&thread_go);
    while (!get_flag(&thread_unregistered)) {
      GC_start_incremental_collection();
      /* Let the thread take the allocator lock too. */
#  ifdef YIELD_20MS
      yield_20ms();
#  else
      sched_yield();
#  endif
    }
    CHECK(pthread_join(t, NULL) == 0);
    /*
     * Any read of the stack of the thread faults now.  Its pages are
     * released first (if the system does it).
     */
#  ifdef MADV_DONTNEED
    (void)madvise(stack, STACK_BYTES, MADV_DONTNEED);
#  endif
    CHECK(mprotect(stack, STACK_BYTES, PROT_NONE) == 0);
    if (unfinished_at_unregister) {
      unfinished_rounds++;
      (void)allocate_until(0);
    }
  }
  GC_gcollect();
  for (round = 0; round < N_ROUNDS; round++)
    CHECK(munmap(stacks[round], STACK_BYTES) == 0);
  printf("SUCCEEDED: %d rounds, %d unregistered during a mark,"
         " incremental %d\n",
         N_ROUNDS, unfinished_rounds, GC_is_incremental_mode());
  return 0;
}

#else

int
main(void)
{
#  if defined(GC_USE_DLOPEN_WRAP) || defined(GC_USE_LD_WRAP)
  printf("Skipped: pthread_create() may register the thread\n");
#  elif defined(NO_CLOCK)
  printf("Skipped: no time limit of a world-stopped mark\n");
#  else
  printf("Skipped: needs POSIX threads and mmap\n");
#  endif
  return 0;
}

#endif
