/*
 * Test the number of markers of each mark phase
 * (`GC_set_bytes_per_marker()`): a mark phase estimated to scan less than
 * the bytes per marker is done by the collecting thread only, no more
 * threads mark a phase than its number of markers, each of the allocation
 * and live data terms of the estimate alone gives a mark phase more than
 * one marker, and the live data survives many collections while the
 * number of markers changes and other threads allocate.  A mark phase
 * done by the collecting thread only calls the procedure set by
 * `GC_set_on_mark_stack_empty()` (once, or more after a mark stack
 * overflow), while a parallel mark phase never calls it.  The threads
 * which mark a phase are recorded by the mark procedure of the nodes of
 * the live data.  The roots and the other data a mark phase scans besides
 * the live data of the test differ much between the builds, thus the test
 * measures a bound of the estimate of a mark phase of its live data, and
 * sets its bytes per marker and the size of its big data from it.  Unless
 * the collector supports the bytes per marker and uses parallel marking,
 * the incremental mode is off and there is no time limit, the checks of
 * the kind of a mark phase and of the number of its marking threads are
 * skipped, except for the check that the maximum bytes per marker give
 * a mark phase done by the collecting thread only.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef GC_THREADS
#  define GC_THREADS
#endif

#include "gc.h"
#include "gc/gc_mark.h"

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

/* Number of the threads allocating during the stress step. */
#ifndef N_MUTATORS
#  ifdef NTHREADS
#    define N_MUTATORS NTHREADS
#  else
#    define N_MUTATORS 4
#  endif
#endif

/*
 * Number of the sets of live data: one per allocating thread, and one
 * of the main thread (the last one).
 */
#define N_SETS (N_MUTATORS + 1)
#define MAIN_SET N_MUTATORS

/* Number of the chains of nodes in each set. */
#define N_CHAINS 1024

/* Number of the nodes in each chain. */
#define CHAIN_LEN 4

/* Minimum number of the chains of the big set of live data. */
#define N_BIG_CHAINS ((size_t)1 << 16)

/* Number of the rounds of the check of the number of marking threads. */
#define N_BOUND_ROUNDS 6

/* Number of the collections forced by each of the single-thread steps. */
#define N_FORCED 20

/* Number of the collections the stress step waits for. */
#define N_STRESS_GCS 300

/* Limit of the stress step passes (in case the collections are rare). */
#define MAX_STRESS_PASSES 100000

#define MAGIC ((GC_word)0x5a5a1234UL)

/*
 * The bytes per marker of the first probe of `estimate_bound()`, and the
 * limit of its probes.
 */
#define PROBE_MIN ((size_t)1 << 20)
#define PROBE_MAX ((size_t)1 << 30)

/*
 * The margin added to a bound of the estimate of a mark phase, which
 * covers the variation of the estimate between the collections: the
 * bytes allocated between them (less than 100 KiB in this test), the
 * depth of the stack, the size of an allocated array rounded up to whole
 * heap blocks.
 */
#define MARGIN ((size_t)1 << 20)

/* Size of a big object. */
#define BIG_BYTES ((size_t)1 << 20)

/*
 * Number of the big objects allocated by each of the checks of terms
 * beyond the bytes per marker of these checks.
 */
#define N_BIG_EXTRA 8

/*
 * The minimum bytes per marker of the checks of terms: much more than the
 * estimate of a mark phase of the sets unless the roots are big.
 */
#define TERM_BYTES ((size_t)16 << 20)

/* Maximum number of the big objects allocated by a check of terms. */
#define MAX_BIG ((PROBE_MAX + MARGIN) / BIG_BYTES + N_BIG_EXTRA)

/* A node whose key is a multiple of this records its marking thread. */
#define RECORD_EVERY 16

/* Maximum number of the recorded marking threads of a mark phase. */
#define MAX_RECORDED 64

/*
 * A node of the live data.  Only `next` is a pointer (the mark procedure
 * pushes only it); the padding makes the node big, so that the live data
 * has many bytes per call of the mark procedure.
 */
struct node {
  struct node *next;
  GC_word key;
  GC_word check;
  GC_word pad[13];
};

#ifdef GC_PTHREADS
typedef pthread_t thread_id_t;
#  define THREAD_SELF() pthread_self()
#  define THREAD_EQUAL(a, b) pthread_equal(a, b)
static pthread_mutex_t record_ml = PTHREAD_MUTEX_INITIALIZER;
#  define RECORD_LOCK() CHECK(pthread_mutex_lock(&record_ml) == 0)
#  define RECORD_UNLOCK() CHECK(pthread_mutex_unlock(&record_ml) == 0)
#else
typedef DWORD thread_id_t;
#  define THREAD_SELF() GetCurrentThreadId()
#  define THREAD_EQUAL(a, b) ((a) == (b))
static CRITICAL_SECTION record_cs;
#  define RECORD_LOCK() EnterCriticalSection(&record_cs)
#  define RECORD_UNLOCK() LeaveCriticalSection(&record_cs)
#endif

/*
 * The distinct threads which called the mark procedure for a node whose
 * key is a multiple of `RECORD_EVERY` since the last reset.  Protected by
 * the record lock.  The main thread acquires the lock only while no
 * other client thread exists, so that no collection can stop the main
 * thread while it holds the lock.
 */
static thread_id_t recorded[MAX_RECORDED];
static unsigned n_recorded = 0;

static void
record_marking_thread(void)
{
  thread_id_t self = THREAD_SELF();
  unsigned i;

  RECORD_LOCK();
  for (i = 0; i < n_recorded; i++) {
    if (THREAD_EQUAL(recorded[i], self))
      break;
  }
  if (i == n_recorded && n_recorded < MAX_RECORDED)
    recorded[n_recorded++] = self;
  RECORD_UNLOCK();
}

static void
reset_marking_threads(void)
{
  RECORD_LOCK();
  n_recorded = 0;
  RECORD_UNLOCK();
}

static unsigned
count_marking_threads(void)
{
  unsigned n;

  RECORD_LOCK();
  n = n_recorded;
  RECORD_UNLOCK();
  return n;
}

static struct GC_ms_entry *GC_CALLBACK
node_mark_proc(GC_word *addr, struct GC_ms_entry *mark_stack_top,
               struct GC_ms_entry *mark_stack_limit, GC_word env)
{
  struct node *n = (struct node *)addr;

  (void)env;
  if (n->key % RECORD_EVERY == 0)
    record_marking_thread();
  return GC_MARK_AND_PUSH(n->next, mark_stack_top, mark_stack_limit,
                          (void **)&n->next);
}

/* The object kind of the nodes. */
static int node_kind;

/* The sets of live data (static roots). */
static struct node *live[N_SETS][N_CHAINS];

/*
 * The chain heads of the big set of live data, and the big
 * pointer-containing objects kept live by `check_terms()`.
 * The elements are volatile, so that the stores to them, which are never
 * read, are not removed.
 */
static struct node **volatile big_chains;
static void *volatile big[MAX_BIG];

/*
 * Number of the calls of `on_mark_stack_empty()`, i.e. of the mark
 * phases done by the collecting thread only.  Updated with the allocator
 * lock held.
 */
static volatile GC_word serial_phases = 0;

static struct GC_ms_entry *GC_CALLBACK
on_mark_stack_empty(struct GC_ms_entry *mark_stack_top,
                    struct GC_ms_entry *mark_stack_limit)
{
  (void)mark_stack_limit;
  serial_phases++;
  return mark_stack_top;
}

static void *
checked(void *p)
{
  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}

static GC_word
next_random(GC_word *pseed)
{
  *pseed = *pseed * (GC_word)1103515245UL + (GC_word)12345;
  return *pseed >> 8;
}

/* Allocate `n` objects of 16..512 bytes, which are dropped at once. */
static void
allocate_garbage(GC_word *pseed, int n)
{
  int i;

  for (i = 0; i < n; i++) {
    GC_word r = next_random(pseed);
    size_t lb = 16 + (size_t)(r % (512 - 16 + 1));

    if (((r >> 12) & 1) != 0) {
      (void)checked(GC_MALLOC_ATOMIC(lb));
    } else {
      (void)checked(GC_MALLOC(lb));
    }
  }
}

/*
 * Allocate a chain of nodes with consecutive keys starting at `*pkey`,
 * and advance the latter.
 */
static struct node *
make_chain(GC_word *pkey)
{
  struct node *head = NULL;
  int i;

  for (i = CHAIN_LEN - 1; i >= 0; i--) {
    GC_word key = *pkey + (GC_word)i;
    struct node *n = (struct node *)checked(
        GC_generic_malloc(sizeof(struct node), node_kind));

    n->next = head;
    n->key = key;
    n->check = key ^ MAGIC;
    GC_END_STUBBORN_CHANGE(n);
    GC_reachable_here(head);
    head = n;
  }
  *pkey += CHAIN_LEN;
  return head;
}

static void
fill_set(int id, GC_word *pkey)
{
  int j;

  for (j = 0; j < N_CHAINS; j++) {
    live[id][j] = make_chain(pkey);
  }
}

static void
replace_chain(int id, GC_word *pseed, GC_word *pkey)
{
  live[id][next_random(pseed) % N_CHAINS] = make_chain(pkey);
}

static void
check_chain(const struct node *n)
{
  GC_word key;
  int i;

  CHECK(n != NULL);
  key = n->key;
  for (i = 0; i < CHAIN_LEN; i++) {
    CHECK(n != NULL);
    CHECK(n->key == key + (GC_word)i);
    CHECK(n->check == (n->key ^ MAGIC));
    n = n->next;
  }
  CHECK(NULL == n);
}

static void
check_set(int id)
{
  int j;

  for (j = 0; j < N_CHAINS; j++) {
    check_chain(live[id][j]);
  }
}

/*
 * Set by the main thread to stop the allocating threads.  Protected by
 * the allocator lock.
 */
static int stop_flag = 0;

struct shared_state_s {
  GC_word gc_no;
  int stop;
};

static void *GC_CALLBACK
read_shared_state(void *arg)
{
  struct shared_state_s *ps = (struct shared_state_s *)arg;

  ps->gc_no = GC_get_gc_no();
  ps->stop = stop_flag;
  return NULL;
}

static void *GC_CALLBACK
set_stop_flag(void *arg)
{
  (void)arg;
  stop_flag = 1;
  return NULL;
}

static struct shared_state_s
get_shared_state(void)
{
  struct shared_state_s s;

  (void)GC_call_with_alloc_lock(read_shared_state, &s);
  return s;
}

#if N_MUTATORS > 0
#  ifdef GC_PTHREADS
static void *
mutator(void *arg)
#  else
static DWORD WINAPI
mutator(LPVOID arg)
#  endif
{
  int id = (int)(GC_uintptr_t)arg;
  GC_word seed = (GC_word)id * 7919 + 1;
  GC_word key = (GC_word)(id + 1) << 24;
  unsigned long iter;

  fill_set(id, &key);
  for (iter = 1;; iter++) {
    replace_chain(id, &seed, &key);
    allocate_garbage(&seed, 32);
    if (iter % 64 == 0) {
      check_set(id);
      if (get_shared_state().stop)
        break;
    }
  }
#  ifdef GC_PTHREADS
  return arg;
#  else
  return (DWORD)(GC_uintptr_t)arg;
#  endif
}
#endif

/*
 * Force `N_FORCED` collections with the bytes per marker set to `value`,
 * check that each of them has a mark phase done by the collecting thread
 * only (if `expect_serial`), or that none of them has one (if
 * `expect_parallel`), and that no more than `max_threads` threads mark
 * each of them (if nonzero), and check the set of the main thread.
 * Returns the maximum number of the threads marking one of them.
 */
static unsigned
force_collections(size_t value, int expect_serial, int expect_parallel,
                  unsigned max_threads, GC_word *pseed, GC_word *pkey)
{
  unsigned most = 0;
  int i;

  GC_set_bytes_per_marker(value);
  for (i = 0; i < N_FORCED; i++) {
    GC_word before;
    unsigned n;

    replace_chain(MAIN_SET, pseed, pkey);
    allocate_garbage(pseed, 256);
    before = serial_phases;
    reset_marking_threads();
    GC_gcollect();
    n = count_marking_threads();
    if (n > most)
      most = n;
    if (expect_serial)
      CHECK(serial_phases > before);
    if (expect_parallel)
      CHECK(serial_phases == before);
    if (max_threads != 0)
      CHECK(n <= max_threads);
  }
  check_set(MAIN_SET);
  return most;
}

/*
 * Return a bound of the estimate of a mark phase of the current live
 * data which follows a mark phase done by the collecting thread only (so
 * that the mark counts behind the live data term of the estimate are
 * exact), with almost nothing allocated in between: the smallest of the
 * probed bytes per marker which gives such a mark phase one marker.  The
 * probes start at `PROBE_MIN` and grow by one eighth.  Besides the live
 * data of the test, the estimate counts the roots, the stacks and the
 * objects kept by conservative pointers, which differ much between the
 * builds, e.g. the static data of a sanitizer runtime, or the cached
 * stacks of the exited threads, which are scanned as roots on Linux if
 * `malloc` is redirected.
 */
static size_t
estimate_bound(void)
{
  size_t value;

  for (value = PROBE_MIN;; value += value / 8) {
    GC_word before;

    CHECK(value <= PROBE_MAX);
    GC_set_bytes_per_marker(~(size_t)0);
    GC_gcollect();
    GC_set_bytes_per_marker(value);
    before = serial_phases;
    GC_gcollect();
    if (serial_phases > before)
      return value;
  }
}

/*
 * Check that a mark phase with two markers is marked by two threads at
 * most (if `exact`): the bytes per marker are set to three quarters of
 * the size of a big set of live nodes, which dominates the estimate.
 * Each collection with two markers follows one done by the collecting
 * thread only, whose mark counts (thus the live data term of the
 * estimate) are exact.  So the estimate is at least the size of the set
 * (more than the bytes per marker), and at most one and a half of it,
 * since the rest of the estimate (`bound`, the bound of the estimate
 * without the set, plus the chain heads, plus less than `MARGIN`) is at
 * most half of it, which the number of the chains of the set is chosen
 * for.  Returns the maximum number of the threads marking one of the mark
 * phases with two markers, and stores to `*most_all_big` the one of the
 * mark phases with all the markers (which alternate with the former), for
 * comparison.
 */
static unsigned
check_two_markers(int exact, size_t bound, GC_word *pkey,
                  unsigned *most_all_big)
{
  size_t i, n_chains, chain_bytes, set_bytes, value;
  unsigned most = 0, most_all = 0;
  int round;

  chain_bytes = CHAIN_LEN * GC_size(live[MAIN_SET][0]);
  n_chains
      = 2 * (bound + MARGIN) / (chain_bytes - 2 * sizeof(struct node *)) + 1;
  if (n_chains < N_BIG_CHAINS)
    n_chains = N_BIG_CHAINS;
  big_chains
      = (struct node **)checked(GC_MALLOC(n_chains * sizeof(struct node *)));
  for (i = 0; i < n_chains; i++) {
    big_chains[i] = make_chain(pkey);
  }
  GC_END_STUBBORN_CHANGE(big_chains);
  set_bytes = n_chains * chain_bytes;
  value = set_bytes / 4 * 3;
  for (round = 0; round < N_BOUND_ROUNDS; round++) {
    GC_word before;
    unsigned n;

    GC_set_bytes_per_marker(~(size_t)0);
    GC_gcollect();
    GC_set_bytes_per_marker(value);
    before = serial_phases;
    reset_marking_threads();
    GC_gcollect();
    n = count_marking_threads();
    if (n > most)
      most = n;
    if (exact) {
      CHECK(serial_phases == before);
      CHECK(n <= 2);
    }
    GC_set_bytes_per_marker(0);
    reset_marking_threads();
    GC_gcollect();
    n = count_marking_threads();
    if (n > most_all)
      most_all = n;
  }
  *most_all_big = most_all;
  for (i = 0; i < n_chains; i++) {
    check_chain(big_chains[i]);
  }
  printf("Big set: %lu KiB, bytes per marker: %lu KiB\n",
         (unsigned long)(set_bytes >> 10), (unsigned long)(value >> 10));
  big_chains = NULL;
  return most;
}

/*
 * Check that each of the allocation and live data terms of the estimate
 * alone gives a mark phase more than one marker (if `exact`): with the
 * bytes per marker set to `bound` (the bound of the estimate of a mark
 * phase of the sets) plus `MARGIN`, but at least `TERM_BYTES`, a mark
 * phase of the sets is done by the collecting thread only, while one is
 * parallel after big objects of more than the bytes per marker are
 * allocated with the collections disabled, and one is parallel when big
 * pointer-containing objects of more than the bytes per marker were found
 * live by the previous collection and almost nothing is allocated since
 * then.
 */
static void
check_terms(int exact, size_t bound)
{
  size_t value = bound + MARGIN < TERM_BYTES ? TERM_BYTES : bound + MARGIN;
  size_t i, n_big = value / BIG_BYTES + N_BIG_EXTRA;
  GC_word before;

  GC_set_bytes_per_marker(value);
  GC_gcollect();
  before = serial_phases;
  GC_gcollect();
  if (exact)
    CHECK(serial_phases > before);

  /*
   * Only the bytes allocated since the previous collection are many.
   * The heap is expanded first, so that it does not grow (with
   * a warning) while the collections are disabled.
   */
  (void)GC_expand_hp((n_big + 8) * BIG_BYTES);
  GC_disable();
  for (i = 0; i < n_big; i++) {
    (void)checked(GC_MALLOC_ATOMIC(BIG_BYTES));
  }
  GC_enable();
  before = serial_phases;
  GC_gcollect();
  if (exact)
    CHECK(serial_phases == before);

  /* Only the pointer-containing objects found live are many. */
  for (i = 0; i < n_big; i++) {
    big[i] = checked(GC_MALLOC(BIG_BYTES));
  }
  GC_gcollect();
  before = serial_phases;
  GC_gcollect();
  if (exact)
    CHECK(serial_phases == before);
  for (i = 0; i < n_big; i++) {
    big[i] = NULL;
  }
  printf("Bytes per marker of the checks of terms: %lu KiB\n",
         (unsigned long)(value >> 10));
}

int
main(void)
{
  GC_word seed = 1;
  GC_word key = 0;
  size_t initial, bound, stress_values[4];
  int supported, exact;
  unsigned markers, most_serial, most_all, most_two, most_all_big;
  int i;
  unsigned long pass;
  GC_word gc_no0;
#if N_MUTATORS > 0
#  ifdef GC_PTHREADS
  pthread_t t[N_MUTATORS];
#  else
  HANDLE t[N_MUTATORS];
#  endif
#endif

#ifndef GC_PTHREADS
  InitializeCriticalSection(&record_cs);
#endif
  /* `GC_MARKERS` environment variable overrides this. */
  GC_set_markers_count(4);
  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  if (GC_is_disabled()) {
    printf("Skipped (the collections are disabled)\n");
    return 0;
  }
  /*
   * The checks assume that little is allocated between the collections
   * of a step (see `MARGIN`).  With refills of several heap blocks, the
   * first refill after a collection empties the thread-local free lists
   * left unused a cycle, e.g. during the collections of `estimate_bound()`,
   * and every free list refilled afterwards counts a whole heap block as
   * allocated.  This overrides `GC_TL_REFILL_MAX` environment variable.
   */
  GC_set_tl_refill_max(1);
  GC_start_mark_threads();
  /* This is optional if `pthread_create()` is redirected. */
  GC_allow_register_threads();
  GC_set_on_mark_stack_empty(on_mark_stack_empty);
  node_kind = (int)GC_new_kind(GC_new_free_list(),
                               GC_MAKE_PROC(GC_new_proc(node_mark_proc), 0),
                               0 /* `adjust` */, 1 /* `clear` */);

  initial = GC_get_bytes_per_marker();
  printf("Initial bytes per marker: %lu\n", (unsigned long)initial);
  GC_set_bytes_per_marker(12345);
  supported = GC_get_bytes_per_marker() == 12345;
  markers = (unsigned)GC_get_parallel() + 1;
  exact = supported && markers > 1 && !GC_is_incremental_mode()
          && GC_get_time_limit() == GC_TIME_UNLIMITED;
  printf("Bytes per marker supported: %d, markers: %u, exact checks: %d\n",
         supported, markers, exact);

  fill_set(MAIN_SET, &key);
  /* Every mark phase is done by the collecting thread only. */
  most_serial
      = force_collections(~(size_t)0, 1, 0, exact ? 1 : 0, &seed, &key);
  /* Every mark phase uses all the markers. */
  most_all = force_collections(0, 0, exact, exact ? markers : 0, &seed, &key);
  bound = estimate_bound();
  printf("Bound of the estimate of a mark phase of the main set: %lu KiB\n",
         (unsigned long)(bound >> 10));
  /* The estimate is below this value. */
  (void)force_collections(bound + MARGIN, exact, 0, exact ? 1 : 0, &seed,
                          &key);
  /* The estimate is at least the size of the roots. */
  (void)force_collections(1, 0, exact, exact ? markers : 0, &seed, &key);
  most_two = check_two_markers(exact, bound, &key, &most_all_big);
  printf("Most marking threads in a phase with 1 marker: %u, with 2: %u"
         " (big set), with %u: %u (%u with the big set)\n",
         most_serial, most_two, markers, most_all, most_all_big);

  /*
   * Collect while the bytes per marker change and other threads
   * allocate.
   */
#if N_MUTATORS > 0
  for (i = 0; i < N_MUTATORS; i++) {
#  ifdef GC_PTHREADS
    int err = pthread_create(&t[i], NULL, mutator, (void *)(GC_uintptr_t)i);

    if (err != 0) {
      fprintf(stderr, "Thread #%d creation failed, errno= %d\n", i, err);
      exit(69);
    }
#  else
    DWORD thread_id;

    t[i] = CreateThread(NULL, 0, mutator, (LPVOID)(GC_uintptr_t)i, 0,
                        &thread_id);
    if (NULL == t[i]) {
      fprintf(stderr, "Thread #%d creation failed, errcode= %d\n", i,
              (int)GetLastError());
      exit(69);
    }
#  endif
  }
#endif
  stress_values[0] = 0;
  stress_values[1] = (size_t)1 << 20;
  stress_values[2] = (size_t)4 << 20;
  stress_values[3] = ~(size_t)0;
  gc_no0 = get_shared_state().gc_no;
  for (pass = 0; pass < MAX_STRESS_PASSES; pass++) {
    if (get_shared_state().gc_no - gc_no0 >= N_STRESS_GCS)
      break;
    /* The forced collections also get every value in turn. */
    GC_set_bytes_per_marker(stress_values[(pass + pass / 8) % 4]);
    if (pass % 8 == 0) {
      GC_gcollect();
    } else {
      replace_chain(MAIN_SET, &seed, &key);
      allocate_garbage(&seed, 32);
    }
  }
  (void)GC_call_with_alloc_lock(set_stop_flag, NULL);
#if N_MUTATORS > 0
  for (i = 0; i < N_MUTATORS; i++) {
#  ifdef GC_PTHREADS
    CHECK(pthread_join(t[i], NULL) == 0);
#  else
    CHECK(WaitForSingleObject(t[i], INFINITE) == WAIT_OBJECT_0);
    CloseHandle(t[i]);
#  endif
  }
#endif

  GC_set_bytes_per_marker(initial);
  CHECK(GC_get_bytes_per_marker() == initial);
  GC_gcollect();
  for (i = 0; i < N_SETS; i++) {
    check_set(i);
  }
  bound = estimate_bound();
  printf("Bound of the estimate of a mark phase of the sets: %lu KiB\n",
         (unsigned long)(bound >> 10));
  check_terms(exact, bound);
  printf("Collections: %lu, stress passes: %lu, serial mark phases: %lu\n",
         (unsigned long)GC_get_gc_no(), pass, (unsigned long)serial_phases);
  printf("SUCCEEDED\n");
  return 0;
}
