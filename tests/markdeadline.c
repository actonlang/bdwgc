/*
 * Copyright (c) 2026 Acton contributors
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
 * Test the parallel mark phases that stop at a deadline
 * (`GC_set_mark_deadline()`).  The test includes the collector source.
 *
 * First, `compact_stopped_phase()` runs on crafted global mark stacks.
 * With `STEAL_MARK_STACK_RANGES`, the unclaimed entries of a small ring,
 * at every position and in every number, wrapping around the end of the
 * ring or not, must take the bottom slots, each exactly once.  Without
 * it, the entries no marker has taken (those with a nonzero descriptor)
 * must be packed at the bottom, in order.
 *
 * Then the test marks from the roots with the world stopped (20 times with
 * the parallel marker), as a full collection does, but sets a deadline
 * before every mark phase, at most 50 microseconds after its start: every
 * phase stops, and the next continues from the entries left.  Before each
 * phase, these entries are moved to a global mark stack just larger than
 * needed, which is marked as too small (as a serial push does when the
 * stack fills up).  The trees marked are wide, thus at the deadline
 * a marker often has no room for its local mark stack.  With
 * `STEAL_MARK_STACK_RANGES`, the ring then grows during the stopped phase
 * (a helper may wait for that after the collecting thread has left the
 * phase), no local mark stack is dropped, and the compaction often wraps
 * around the end of the ring.  Without it, the marker marks on, and the
 * global mark stack grows after the phase, keeping the entries.  The test
 * checks that every node of the trees is marked and, with the parallel
 * marker, that these cases happened, the stopped phases, the growths and the
 * wrapped compactions only if the collector's clock resolves the deadlines
 * (on Windows it counts milliseconds).  With `STEAL_MARK_STACK_RANGES`, every
 * phase must have claimed entries, and the stack must have grown in
 * a stopped phase only as a ring.  A deadline 0.9 ms away must not count as
 * passed right after it is set, and a marking with deadlines 60 seconds away
 * must stop no phase.  Then a collection must keep the trees.
 *
 * At last, with the parallel marker, the test turns the incremental mode
 * on (`GC_enable_incremental()`), with every collection full.  With a time
 * limit of 50 ms, a world-stopped mark of a collection must run a parallel
 * mark phase (`GC_mark_no` advances).  With a time limit of 0.1 ms, the
 * world-stopped mark that starts a collection is abandoned, and it must
 * leave no deadline set.  Then a step must be due once `slice_bytes` were
 * allocated since the latest slice, and not earlier (with something
 * allocated since the previous call of `GC_collect_a_little_inner()`),
 * and leave no deadline set.  The checks with the time limit of 0.1 ms are
 * skipped if the clock of the collector does not resolve it (on Windows
 * it counts milliseconds).  A final mark that a step left due must not
 * run with less than `slice_bytes` allocated since the latest slice and
 * the time limit not passed since that step, but once the time limit has
 * passed, and not while the collections are disabled, when
 * `GC_collect_a_little()` must report no work left.  The set-up of a full
 * collection then runs in chunks with a time limit of 1 ns (see
 * `continue_full_setup()`): a chunk must stop before the set-up is
 * complete (except on Windows, where the clock of the collector counts
 * milliseconds, and with `BSD_TIME`, where it reads the user time of the
 * process), the first world-stopped mark must be due after the chunk
 * that completes it, and no block but those of uncollectable objects may
 * keep a mark bit then.  The next chunk must be due once `slice_bytes`
 * were allocated since the latest one, and not earlier, or once the time
 * limit has passed since its end, and none may run while the collections
 * are disabled.  With a time limit of 10 s, the due first world-stopped
 * mark must run, and the first world-stopped mark of a set-up must follow
 * in the chunk that completes it.  With POSIX threads, the clearing of the
 * mark bits and the first world-stopped mark must wait for a builder of
 * a free list.  With memory unmapping (except on Windows), the blocks of
 * every other one of many objects (with resident pages) are freed
 * explicitly and unmapped after the finish of the next collection, in
 * chunks with a time limit of 10 us (the unmapping threshold is one
 * collection): it must not start in a chunk after half of the time limit,
 * and with a time limit of 10 s the next chunk must not be due, even after
 * the allocation volume between two steps.  With the next chunk not due,
 * a collection that is due after the first chunk must end the unmapping
 * and start, but not while none is due, not before that chunk, and not
 * while the collections are disabled.  With the blocks of every other one
 * of as many new objects freed, the unmapping of every free block before
 * a heap growth must stop at the time limit, counted from its start, which
 * leaves most of those blocks mapped, and without a time limit the next
 * growth must unmap more, so that the two together unmap at least half as
 * many bytes as were freed.  The checks of the unmapping run only where
 * the clock of the collector resolves fractions of a millisecond (see
 * `fine_clock()`).  The trees must stay intact.
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
#if GC_GNUC_PREREQ(4, 6) && defined(MSWIN32) && !defined(NO_DEBUGGING) \
    && !defined(LINT2)
/*
 * In this configuration `ABORT()` returns (after `DebugBreak()`), thus
 * gcc (mingw-w64) may follow a failed assertion of the collector, e.g.
 * that `kind` is less than `MAXOBJKINDS`, and report an index out of the
 * bounds of an array after it.
 */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#  define ARRAY_BOUNDS_IGNORED
#endif
#include "../extra/gc.c"
#ifdef ARRAY_BOUNDS_IGNORED
#  pragma GCC diagnostic pop
#endif
#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
#  pragma GCC diagnostic pop
#endif

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#ifdef PARALLEL_MARK_DEADLINE

/* The entries of the crafted mark stacks. */
#  define TEST_STACK_SIZE 64

/* The least size of the global mark stack at the start of a phase. */
#  define SMALL_STACK_SIZE 64

/* The children of a node of the trees, and the levels below a root. */
#  define FANOUT 32
#  define DEPTH 3

/* The trees, and the markings with deadlines. */
#  define N_TREES 4
#  define MAX_MARKINGS 20

struct node {
  GC_word id;
  struct node *child[FANOUT];
};

static struct node *volatile trees[N_TREES];

/* The nodes of a tree. */
static unsigned long nodes_per_tree;

/* The fake entry for number `k` of the crafted ring. */
static void
set_entry(mse *e, AO_t k)
{
  e->mse_start = MAKE_CPTR(16 * (word)(k + 1));
  e->mse_descr = (word)k + 1;
}

static void
check_compaction_at(AO_t first, size_t n)
{
#  ifdef STEAL_MARK_STACK_RANGES
  unsigned char seen[TEST_STACK_SIZE];
  size_t i;
  AO_t k;

  /* Fill every slot, with entries of other numbers outside the range. */
  for (i = 0; i < TEST_STACK_SIZE; i++)
    set_entry(&GC_mark_stack[i], first + 1000 + i);
  for (k = first; k != first + n; k++)
    set_entry(&GC_mark_stack[MARK_RING_INDEX(k)], k);
#    ifdef CHECK_MARK_RING
  AO_store(&GC_n_claimed, first);
#    endif
  AO_store(&GC_first_unclaimed, first);
  AO_store(&GC_n_published, first + n);
  compact_stopped_phase();
  CHECK(GC_mark_stack_top == GC_mark_stack + n - 1);
  memset(seen, 0, sizeof(seen));
  for (i = 0; i < n; i++) {
    AO_t e = (AO_t)GC_mark_stack[i].mse_descr - 1;

    CHECK((word)(e - first) < (word)n);
    CHECK(GC_mark_stack[i].mse_start == MAKE_CPTR(16 * (word)(e + 1)));
    CHECK(!seen[e - first]);
    seen[e - first] = 1;
  }
#  else
  size_t i;
  mse *q = GC_mark_stack;

  /*
   * Here `first` is the index of `GC_first_nonempty`, and `n` that of the
   * top entry plus one.  Every third entry is taken.
   */
  for (i = 0; i < TEST_STACK_SIZE; i++) {
    set_entry(&GC_mark_stack[i], i);
    if (i % 3 == 1)
      GC_mark_stack[i].mse_descr = 0;
  }
  GC_cptr_store(&GC_first_nonempty, (ptr_t)(GC_mark_stack + first));
  GC_mark_stack_top = GC_mark_stack + n - 1;
  compact_stopped_phase();
  for (i = (size_t)first; i < n; i++) {
    if (i % 3 != 1) {
      CHECK(ADDR_GE((ptr_t)GC_mark_stack_top, (ptr_t)q));
      CHECK(q->mse_descr == (word)i + 1);
      CHECK(q->mse_start == MAKE_CPTR(16 * (word)(i + 1)));
      q++;
    }
  }
  CHECK(GC_mark_stack_top == q - 1);
#  endif
}

static void
check_compaction(void)
{
  size_t f, n;

  LOCK();
  alloc_mark_stack(TEST_STACK_SIZE);
  CHECK(TEST_STACK_SIZE == GC_mark_stack_size);
#  ifdef STEAL_MARK_STACK_RANGES
  /* The entry numbers start beyond a few sizes of the ring. */
  for (f = 0; f < TEST_STACK_SIZE; f++) {
    for (n = 0; n <= TEST_STACK_SIZE; n++)
      check_compaction_at((AO_t)(3 * TEST_STACK_SIZE + f), n);
  }
#  else
  for (f = 0; f <= TEST_STACK_SIZE; f++) {
    for (n = f; n <= TEST_STACK_SIZE; n++)
      check_compaction_at((AO_t)f, n);
  }
#  endif
  alloc_mark_stack(INITIAL_MARK_STACK_SIZE);
  GC_after_marking();
  UNLOCK();
  printf("Compaction of a stopped phase checked\n");
}

/* The tries at a deadline 0.9 ms away. */
#  define NEAR_DEADLINE_TRIES 100

/*
 * Check that a deadline 0.9 ms away has not passed right after it is set,
 * thus that its nanoseconds count.  A try fails if the thread is preempted
 * meanwhile, thus one of `NEAR_DEADLINE_TRIES` tries must succeed.
 */
static void
check_near_deadline(void)
{
  int failed;

  LOCK();
  for (failed = 0; failed < NEAR_DEADLINE_TRIES; failed++) {
    GET_TIME(GC_start_time);
    GC_set_mark_deadline(0, 900000);
    if (!mark_should_stop())
      break;
  }
  GC_clear_mark_deadline();
  UNLOCK();
  printf("Deadline 0.9 ms away checked, %d tries failed\n", failed);
  CHECK(failed < NEAR_DEADLINE_TRIES);
}

/*
 * Whether the clock of the collector resolves fractions of a millisecond:
 * one of a few tries must see it advance by less than 0.1 ms (a try fails
 * if the thread is preempted meanwhile).  On Windows it counts
 * milliseconds: a phase stops at a deadline less than 1 ms after its start
 * only when the clock advances, and a world-stopped mark with a time limit
 * of 0.1 ms runs until then, often to the end of its marking.  With
 * `BSD_TIME` the result is false without a try: the clock reads the user
 * time of the process (`getrusage()`), which advances in small steps in
 * such a loop but may stay the same over a whole world-stopped mark.
 */
static GC_bool
fine_clock(void)
{
#  ifdef BSD_TIME
  return FALSE;
#  else
  int i;

  for (i = 0; i < 10; i++) {
    CLOCK_TYPE start, now;
    unsigned long us;

    GET_TIME(start);
    do {
      GET_TIME(now);
      us = MS_TIME_DIFF(now, start) * 1000UL
           + NS_FRAC_TIME_DIFF(now, start) / 1000UL;
    } while (0 == us);
    if (us < 100)
      return TRUE;
  }
  return FALSE;
#  endif
}

static struct node *
make_tree(GC_word id, int level)
{
  struct node *n = GC_NEW(struct node);

  if (NULL == n) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  n->id = id;
  if (level < DEPTH) {
    int i;

    for (i = 0; i < FANOUT; i++) {
      struct node *c = make_tree(id * FANOUT + (GC_word)i + 1, level + 1);

      GC_ptr_store_and_dirty(&n->child[i], c);
    }
  }
  return n;
}

/*
 * Check the ids of the nodes of the tree, and that they are marked if
 * `want_marked`; return the number of the nodes.
 */
static unsigned long
check_tree(const struct node *n, GC_word id, int level, int want_marked)
{
  unsigned long count = 1;
  int i;

  CHECK(n != NULL && n->id == id);
  if (want_marked)
    CHECK(GC_is_marked(GC_base((void *)n)));
  for (i = 0; i < FANOUT; i++) {
    if (level < DEPTH) {
      count += check_tree(n->child[i], id * FANOUT + (GC_word)i + 1, level + 1,
                          want_marked);
    } else {
      CHECK(NULL == n->child[i]);
    }
  }
  return count;
}

/*
 * Move the entries of the global mark stack to a new one, whose size is
 * the smallest power of two above the number of the entries, and at least
 * `SMALL_STACK_SIZE`, if that is smaller.  Mark the stack as too small, as
 * a serial push does when the stack is nearly full: it should grow once it
 * is empty, not after a phase stopped at the deadline (unless a marker had
 * no room for its local mark stack then).
 */
static void
shrink_mark_stack(void)
{
  size_t n = (size_t)(GC_mark_stack_top + 1 - GC_mark_stack);
  size_t size = SMALL_STACK_SIZE;
  mse *new_stack;

  while (size <= n)
    size *= 2;
  if (size < GC_mark_stack_size) {
    new_stack = (mse *)GC_scratch_alloc(mark_stack_bytes(size));
    CHECK(new_stack != NULL);
    BCOPY(GC_mark_stack, new_stack, n * sizeof(mse));
    set_mark_stack(new_stack, size, FALSE);
    GC_mark_stack_top = new_stack + n - 1;
  }
  GC_mark_stack_too_small = TRUE;
}

/* The cases a marking met, summed over the markings. */
static unsigned long n_stopped_phases;
static unsigned long n_grown_in_stopped;
static unsigned long n_wrapped;
static unsigned long n_stacks_dropped;

/*
 * Mark from the roots, with the world stopped, with a deadline set before
 * every call of `GC_mark_some()`, and check that every node of the trees
 * is marked.  The deadlines are `far_ms` milliseconds after the start of
 * every call if `far_ms` is nonzero.  Otherwise they are at most 50
 * microseconds after it, and the global mark stack is shrunk before every
 * mark phase.  Phases without a near deadline run to their end on the mark
 * stack the collector sizes: without `STEAL_MARK_STACK_RANGES`, a phase
 * does not use the slots of the entries taken from the global mark stack
 * again, thus the parts of large root ranges which its markers share may
 * overflow a shrunk one in every phase, and the next shrink would undo the
 * growth of the stack after the overflow.
 */
static void
mark_with_deadlines(unsigned long far_ms)
{
  /* The deadlines, in nanoseconds after the start of a phase, in turn. */
  static const unsigned long deadline_ns[] = { 0, 2000, 10000, 50000 };
  const unsigned n_deadlines = sizeof(deadline_ns) / sizeof(deadline_ns[0]);
  ptr_t cold_gc_frame = GC_approx_sp();
  unsigned calls = 0;
  int t;
  IF_CANCEL(int cancel_state;)

  LOCK();
  /* The markers wait with the thread cancellation disabled. */
  DISABLE_CANCEL(cancel_state);
  ENTER_GC();
  /* The builders of free lists read the mark bits without the lock. */
  GC_wait_for_reclaim();
  (void)GC_reclaim_all((GC_stop_func)0, FALSE);
  GC_invalidate_mark_state();
  GC_clear_marks();
  STOP_WORLD();
#  ifdef THREAD_LOCAL_ALLOC
  GC_world_stopped = TRUE;
#  endif
  GC_initiate_gc();
  for (;;) {
    GC_bool phase = GC_mark_state == MS_ROOTS_PUSHED && GC_parallel
                    && GC_phase_markers > 1;
    word ring_grown, grown, no_room;

    if (phase && 0 == far_ms)
      shrink_mark_stack();
    ring_grown = GC_mark_stack_events.n_ring_grown;
    grown = GC_mark_stack_events.n_grown;
    no_room = GC_mark_stack_events.n_no_room;
    GET_TIME(GC_start_time);
    if (far_ms != 0) {
      GC_set_mark_deadline(far_ms, 0);
    } else {
      GC_set_mark_deadline(0, deadline_ns[calls++ % n_deadlines]);
    }
    if (GC_mark_some(cold_gc_frame)) {
      GC_clear_mark_deadline();
      break;
    }
    GC_clear_mark_deadline();
    n_stacks_dropped
        += (unsigned long)(GC_mark_stack_events.n_no_room - no_room);
    if (!phase)
      continue;
    if (ADDR_LT((ptr_t)GC_mark_stack_top, (ptr_t)GC_mark_stack)) {
      /* The phase ended, and dropped entries. */
      CHECK(GC_mark_state == MS_INVALID);
      continue;
    }
    /*
     * The phase stopped at the deadline, with entries left.  Without
     * `STEAL_MARK_STACK_RANGES`, a marker may also have dropped its local
     * mark stack for lack of room, before the deadline or after it kept
     * the stack at the deadline and marked on, which invalidates the mark
     * state but does not prevent the growth after the phase.
     */
    n_stopped_phases++;
#  ifdef STEAL_MARK_STACK_RANGES
    /* Every phase marks from a range at least, after its deadline too. */
    CHECK(AO_load(&GC_first_unclaimed) > 0);
    /* The stack grew as a ring only. */
    CHECK(GC_mark_stack_events.n_grown - grown
          == GC_mark_stack_events.n_ring_grown - ring_grown);
    if (GC_mark_stack_events.n_ring_grown != ring_grown)
      n_grown_in_stopped++;
    {
      AO_t first = AO_load(&GC_first_unclaimed);
      size_t n = (size_t)(AO_load(&GC_n_published) - first);

      /* The unclaimed entries wrapped around the end of the ring. */
      if (MARK_RING_INDEX(first) + n > GC_mark_stack_size)
        n_wrapped++;
    }
#  else
    if (GC_mark_stack_events.n_grown != grown)
      n_grown_in_stopped++;
    (void)ring_grown;
#  endif
  }
  for (t = 0; t < N_TREES; t++)
    CHECK(check_tree(trees[t], (GC_word)t + 1, 0, 1) == nodes_per_tree);
#  ifdef THREAD_LOCAL_ALLOC
  GC_world_stopped = FALSE;
#  endif
  START_WORLD();
  GC_after_marking();
  EXIT_GC();
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/* The calls of `GC_collect_a_little()` which a collection takes at most. */
#  define MAX_COLLECT_CALLS 100000L

/* The size of the objects allocated to pass `slice_bytes`. */
#  define ALLOC_CHUNK_BYTES 65536

/*
 * The world-stopped marks (from `GC_EVENT_MARK_START` until the world is
 * restarted), and those of them which ran a parallel mark phase.  Updated
 * by `on_collection_event()`, which the collector calls holding the
 * allocator lock.
 */
static GC_bool in_stopped_mark;
static word mark_no_at_mark_start;
static unsigned long n_stopped_marks;
static unsigned long n_parallel_stopped_marks;

/* The world stops, counted by `on_collection_event()` too. */
static unsigned long n_stops;

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
  case GC_EVENT_POST_STOP_WORLD:
    n_stops++;
    break;
  case GC_EVENT_MARK_START:
    in_stopped_mark = TRUE;
    mark_no_at_mark_start = GC_mark_no;
    break;
  case GC_EVENT_PRE_START_WORLD:
    if (in_stopped_mark) {
      in_stopped_mark = FALSE;
      n_stopped_marks++;
      if (GC_mark_no != mark_no_at_mark_start)
        n_parallel_stopped_marks++;
    }
    break;
  default:
    break;
  }
}

static void
set_time_limit(unsigned long ms, unsigned long ns)
{
  struct GC_timeval_s tv;

  tv.tv_ms = ms;
  tv.tv_nsec = ns;
  GC_set_time_limit_tv(tv);
}

/* Spin until `us` microseconds have passed since `start`. */
static void
wait_us_since(CLOCK_TYPE start, unsigned long us)
{
  while (us_since(start) < us) {
    /* Empty. */
  }
}

/*
 * Start a collection and run its set-up (see `SETUP_PENDING()`) until its
 * first world-stopped mark: every collection is full.
 */
static void
start_collection(void)
{
  IF_CANCEL(int cancel_state;)

  GC_start_incremental_collection();
  LOCK();
  DISABLE_CANCEL(cancel_state);
  while (SETUP_PENDING())
    continue_full_setup();
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/*
 * Complete the collection in progress, if any, and its finish, which
 * nothing allocated makes due only by the time limit.
 */
static void
complete_collection(void)
{
  long n;
  IF_CANCEL(int cancel_state;)

  for (n = 0; GC_collect_a_little() && !FINISH_PENDING(); n++)
    CHECK(n < MAX_COLLECT_CALLS);
  LOCK();
  DISABLE_CANCEL(cancel_state);
  GC_complete_finish();
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/* Check that the world-stopped marks with the time limit mark in parallel. */
static void
check_stopped_marks(void)
{
  set_time_limit(50, 0);
  start_collection();
  complete_collection();
  printf("Collection with a time limit of 50 ms: %lu world-stopped marks,"
         " %lu with a parallel mark phase\n",
         n_stopped_marks, n_parallel_stopped_marks);
  CHECK(n_parallel_stopped_marks > 0);
}

/*
 * Check that with a time limit of 0.1 ms the world-stopped mark that starts
 * a collection is abandoned, and leaves no deadline set.
 */
static void
check_abandoned_mark(void)
{
  set_time_limit(0, 100000);
  start_collection();
  LOCK();
  CHECK(GC_collection_in_progress());
  CHECK(!GC_mark_deadline_on);
  UNLOCK();
  complete_collection();
}

/*
 * Check that a step is due once `slice_bytes` were allocated since the
 * latest slice, and not earlier if something was allocated since the
 * previous call of `GC_collect_a_little_inner()`, and that a step leaves
 * no deadline set.
 */
static void
check_step_trigger(void)
{
  unsigned long stops;
  GC_bool enough;

  /* The time limit is 0.1 ms, thus the collection stays in progress. */
  start_collection();
  /* Allocate more than `slice_bytes`, without marking. */
  GC_disable();
  do {
    CHECK(GC_MALLOC_ATOMIC(ALLOC_CHUNK_BYTES) != NULL);
    LOCK();
    enough = allocd_now() > slice_bytes;
    UNLOCK();
  } while (!enough);
  GC_enable();

  LOCK();
  CHECK(GC_collection_in_progress());
  CHECK(slice_bytes > 0);
  /*
   * One byte short of `slice_bytes` since the latest slice, and some since
   * the previous call.
   */
  allocd_at_slice = allocd_now() - (slice_bytes - 1);
  allocd_at_call = allocd_now() - 1;
  stops = n_stops;
  GC_collect_a_little_inner(1);
  CHECK(n_stops == stops);
  /* Exactly `slice_bytes` since the latest slice. */
  allocd_at_slice = allocd_now() - slice_bytes;
  allocd_at_call = allocd_now() - 1;
  GC_collect_a_little_inner(1);
  CHECK(n_stops > stops);
  CHECK(!GC_mark_deadline_on);
  UNLOCK();
  complete_collection();
}

/* A stop function that always stops. */
static int GC_CALLBACK
stop_now(void)
{
  return TRUE;
}

/*
 * Check that a final mark that a step left due runs as a step would, after
 * `slice_bytes`, or once the time limit has passed since that step, but
 * not while the collections are disabled.
 */
static void
check_final_trigger(void)
{
  unsigned long stops;
  GC_bool enough;
  IF_CANCEL(int cancel_state;)

  /*
   * The stop function stops the first world-stopped mark of the collection
   * at once, also where the clock of the collector counts milliseconds.
   */
  set_time_limit(0, 100000);
  GC_set_stop_func(stop_now);
  start_collection();
  GC_set_stop_func(GC_never_stop_func);
  LOCK();
  DISABLE_CANCEL(cancel_state);
  /* Run steps, nothing allocated in between, until the final mark is due. */
  while (!GC_final_mark_due) {
    CHECK(GC_collection_in_progress());
    allocd_at_call = allocd_now();
    GC_collect_a_little_inner(1);
  }
  RESTORE_CANCEL(cancel_state);
  UNLOCK();

  /* Allocate more than `slice_bytes`, with the final mark left due. */
  GC_disable();
  do {
    CHECK(GC_MALLOC_ATOMIC(ALLOC_CHUNK_BYTES) != NULL);
    LOCK();
    enough = allocd_now() > slice_bytes;
    UNLOCK();
  } while (!enough);
  CHECK(!GC_collect_a_little());
  LOCK();
  DISABLE_CANCEL(cancel_state);
  /* No marking is left to complete while the collections are disabled. */
  CHECK(GC_complete_marking(stop_now, 1));
  CHECK(GC_final_mark_due);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  GC_enable();

  LOCK();
  DISABLE_CANCEL(cancel_state);
  /*
   * One byte short of `slice_bytes` since the latest slice, some since the
   * previous call, and the time limit (10 s) not passed since the step.
   */
  set_time_limit(10000, 0);
  GET_TIME(final_due_since);
  allocd_at_slice = allocd_now() - (slice_bytes - 1);
  allocd_at_call = allocd_now() - 1;
  stops = n_stops;
  GC_collect_a_little_inner(1);
  CHECK(n_stops == stops && GC_final_mark_due);
  /* The time limit (1 us) has passed. */
  set_time_limit(0, 1000);
  wait_us_since(final_due_since, 2);
  allocd_at_slice = allocd_now() - (slice_bytes - 1);
  allocd_at_call = allocd_now() - 1;
  GC_collect_a_little_inner(1);
  CHECK(n_stops > stops && !GC_final_mark_due);
  set_time_limit(0, 100000);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  complete_collection();
  printf("Trigger of a due final mark checked\n");
}

/* Count the blocks with marked objects, except uncollectable ones. */
static void
count_marked_block(hdr *hhdr, void *client_data)
{
  if (!IS_UNCOLLECTABLE(hhdr->hb_obj_kind) && hhdr->hb_n_marks != 0)
    ++*(unsigned long *)client_data;
}

/*
 * Check the chunks of the set-up of a full collection, started as
 * `GC_maybe_gc()` starts it, and when the first world-stopped mark of the
 * collection runs.
 */
static void
check_setup_chunks(void)
{
  unsigned long n_marked_blocks = 0;
  unsigned long marks;
  GC_bool enough;
  IF_CANCEL(int cancel_state;)

  complete_collection();
  LOCK();
  DISABLE_CANCEL(cancel_state);
  CHECK(!GC_collection_in_progress() && !SETUP_PENDING());
  /* A chunk stops once it reads the clock. */
  set_time_limit(0, 1);
  setup_phase = SETUP_SWEEP;
  continue_full_setup();
#  if !defined(_WIN32) && !defined(BSD_TIME)
  /*
   * The clock of the collector counts milliseconds on Windows
   * (`GetTickCount64()`), and with `BSD_TIME` it reads the user time of the
   * process (`getrusage()`), which may stay the same over several reads,
   * thus a time limit of 1 ns lasts until the clock advances there.
   */
  CHECK(SETUP_SWEEP == setup_phase || SETUP_CLEAR == setup_phase);
#  endif
  while (SETUP_SWEEP == setup_phase || SETUP_CLEAR == setup_phase)
    continue_full_setup();
  /* The chunk that completed the set-up took more than half the limit. */
  CHECK(SETUP_MARK == setup_phase);
  GC_apply_to_all_hdrs(count_marked_block, &n_marked_blocks);
  CHECK(0 == n_marked_blocks);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();

  /* Allocate more than `slice_bytes`, with the first mark left due. */
  GC_disable();
  do {
    CHECK(GC_MALLOC_ATOMIC(ALLOC_CHUNK_BYTES) != NULL);
    LOCK();
    enough = allocd_now() > slice_bytes;
    UNLOCK();
  } while (!enough);
  CHECK(!GC_collect_a_little());
  GC_enable();

  LOCK();
  DISABLE_CANCEL(cancel_state);
  CHECK(SETUP_MARK == setup_phase);
  CHECK(slice_bytes > 0);
  /*
   * With a time limit of 10 s, the next chunk is due once `slice_bytes`
   * were allocated since the latest one, and not earlier.
   */
  set_time_limit(10000, 0);
  GET_TIME(setup_chunk_end);
  allocd_at_slice = allocd_now() - (slice_bytes - 1);
  CHECK(!setup_chunk_due());
  allocd_at_slice = allocd_now() - slice_bytes;
  CHECK(setup_chunk_due());
  /* With nothing allocated, it is due once the time limit has passed. */
  set_time_limit(0, 1000);
  allocd_at_slice = allocd_now();
  GET_TIME(setup_chunk_end);
  wait_us_since(setup_chunk_end, 2);
  CHECK(setup_chunk_due());

  /* With a time limit of 10 s, the first mark that is due runs. */
  set_time_limit(10000, 0);
  marks = n_stopped_marks;
  continue_full_setup();
  CHECK(!SETUP_PENDING() && n_stopped_marks > marks);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  complete_collection();

  /* The first mark follows in the chunk that completes the set-up. */
  LOCK();
  DISABLE_CANCEL(cancel_state);
  marks = n_stopped_marks;
  setup_phase = SETUP_SWEEP;
  continue_full_setup();
  CHECK(!SETUP_PENDING() && n_stopped_marks > marks);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  complete_collection();
  set_time_limit(0, 100000);
  printf("Chunks of the set-up of a full collection checked\n");
}

#  ifdef GC_PTHREADS
/*
 * Whether the builder has registered itself, and the set-up phase and the
 * world stops that it saw when it left.
 */
static volatile int builder_entered;
static volatile int builder_saw_phase;
static volatile unsigned long builder_saw_stops;

/*
 * A builder of a free list: it registers itself as one, as
 * `GC_generic_malloc_many()` does, and leaves 20 ms later.
 */
static void *
builder(void *arg)
{
  CLOCK_TYPE start;

  (void)arg;
  LOCK();
  GC_fl_builder_enter();
  UNLOCK();
  builder_entered = 1;
  GET_TIME(start);
  wait_us_since(start, 20000);
  builder_saw_phase = setup_phase;
  builder_saw_stops = n_stops;
  GC_fl_builder_leave();
  return NULL;
}

/*
 * Run the set-up from `phase` with a time limit of 10 s, which completes
 * it with the first world-stopped mark, while a builder of a free list
 * works, and return the set-up phase that the builder saw when it left.
 * The world must not stop before the builder leaves.
 */
static int
setup_with_builder(int phase)
{
  pthread_t th;
  unsigned long stops;
  IF_CANCEL(int cancel_state;)

  set_time_limit(10000, 0);
  builder_entered = 0;
  CHECK(pthread_create(&th, NULL, builder, NULL) == 0);
  while (!builder_entered) {
    /* Empty. */
  }
  LOCK();
  DISABLE_CANCEL(cancel_state);
  stops = n_stops;
  setup_phase = phase;
  continue_full_setup();
  CHECK(!SETUP_PENDING());
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  CHECK(pthread_join(th, NULL) == 0);
  CHECK(builder_saw_stops == stops);
  complete_collection();
  return builder_saw_phase;
}

/*
 * Check that the clearing of the mark bits and the first world-stopped
 * mark of a set-up wait for the builders of free lists, which read the
 * mark bits without the allocator lock.
 */
static void
check_setup_builders(void)
{
  IF_CANCEL(int cancel_state;)

  complete_collection();
  CHECK(SETUP_SWEEP == setup_with_builder(SETUP_SWEEP));
  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(0, 1);
  setup_phase = SETUP_SWEEP;
  while (SETUP_SWEEP == setup_phase || SETUP_CLEAR == setup_phase)
    continue_full_setup();
  CHECK(SETUP_MARK == setup_phase);
  /* No chunk is due by the time limit until the builder works. */
  set_time_limit(10000, 0);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  (void)setup_with_builder(SETUP_MARK);
  set_time_limit(0, 100000);
  printf("Builders of free lists during the set-up checked\n");
}
#  endif

#  if defined(USE_MUNMAP) && !defined(_WIN32)
/* The objects allocated in a row, every other one freed. */
#    define N_APART 256

static void *volatile apart[N_APART];

/*
 * Allocate `N_APART` objects of 4 pages in a row, in a heap grown for
 * them and with the collections disabled, writing every page, and free
 * every other one, so that their free blocks stay apart.  By `GC_free()`,
 * as they are allocated without a debug header: `GC_FREE()` is
 * `GC_debug_free()` with `GC_DEBUG`, which leaves a collectable object to
 * the collector, so no block would be freed.  Return the bytes freed.
 */
static size_t
free_apart(void)
{
  size_t bytes = 4 * HBLK_PAGE_SIZE - EXTRA_BYTES;
  GC_warn_proc warn_proc;
  int i;

  CHECK(GC_expand_hp(N_APART * (bytes + HBLKSIZE)));
  GC_disable();
  for (i = 0; i < N_APART; i++) {
    char *p = (char *)GC_malloc_atomic(bytes);
    size_t j;

    CHECK(p != NULL);
    for (j = 0; j < bytes; j += GC_page_size)
      p[j] = 1;
    apart[i] = p;
  }
  for (i = 0; i < N_APART; i += 2) {
    GC_free(apart[i]);
    apart[i] = NULL;
  }
  /* Silence the warning that the heap grew while collections were off. */
  warn_proc = GC_get_warn_proc();
  GC_set_warn_proc(GC_ignore_warn_proc);
  GC_enable();
  GC_set_warn_proc(warn_proc);
  return N_APART / 2 * bytes;
}

/*
 * Check when a chunk of the unmapping may start and is due, that
 * a collection that is due ends the unmapping in chunks after its first
 * chunk, and only then, and that the unmapping before a heap growth stops
 * at the time limit, counted from its start.  Run only where the clock of
 * the collector resolves fractions of a millisecond (see `fine_clock()`):
 * the unmapping does not stop at a time limit of 10 us otherwise.
 */
static void
check_unmap_chunks(void)
{
  unsigned saved_threshold;
  unsigned long marks, since_start;
  size_t freed;
  word unmapped, saved_allocd;
  CLOCK_TYPE before;
  long n;
  int i;
  IF_CANCEL(int cancel_state;)

  complete_collection();
  (void)free_apart();
  LOCK();
  /* The blocks freed before the next collection are old enough then. */
  saved_threshold = GC_unmap_threshold;
  GC_unmap_threshold = 1;
  UNLOCK();
  set_time_limit(0, 10000);
  start_collection();
  for (n = 0; GC_collect_a_little() && !FINISH_PENDING(); n++)
    CHECK(n < MAX_COLLECT_CALLS);
  LOCK();
  DISABLE_CANCEL(cancel_state);
  while (FINISH_PENDING()
         && !(FINISH_UNMAP == finish_phase && unmap_chunk_ran)) {
    GET_TIME(GC_start_time);
    run_finish_chunk(FALSE);
  }
  /* A chunk unmaps a few of the blocks at most. */
  CHECK(FINISH_UNMAP == finish_phase);
  /* The unmapping does not start in a chunk after half of its time limit. */
  unmapped = GC_unmapped_bytes;
  GET_TIME(GC_start_time);
  wait_us_since(GC_start_time, 6);
  run_finish_chunk(FALSE);
  CHECK(FINISH_UNMAP == finish_phase && GC_unmapped_bytes == unmapped);

  /*
   * With a time limit of 10 s, the next chunk of the unmapping is not due,
   * even after the allocation volume between two steps.
   */
  set_time_limit(10000, 0);
  saved_allocd = allocd_at_slice;
  allocd_at_slice = allocd_now() - slice_bytes;
  CHECK(!finish_chunk_due());
  allocd_at_slice = saved_allocd;
  marks = n_stopped_marks;
  GC_collect_a_little_inner(1);
  CHECK(FINISH_UNMAP == finish_phase);
  /* A collection due does not end the unmapping before its first chunk. */
  GC_should_start_incremental_collection = TRUE;
  unmap_chunk_ran = FALSE;
  GC_collect_a_little_inner(1);
  CHECK(FINISH_UNMAP == finish_phase);
  /* Nor while the collections are disabled. */
  unmap_chunk_ran = TRUE;
  GC_dont_gc++;
  GC_collect_a_little_inner(1);
  GC_dont_gc--;
  CHECK(FINISH_UNMAP == finish_phase && n_stopped_marks == marks);
  /* Otherwise it ends the unmapping and starts. */
  GC_collect_a_little_inner(1);
  CHECK(!FINISH_PENDING() && n_stopped_marks > marks);
  GC_unmap_threshold = saved_threshold;
  set_time_limit(0, 100000);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  for (i = 0; i < N_APART; i++)
    apart[i] = NULL;
  complete_collection();

  /*
   * The unmapping of every free block before a heap growth stops at the
   * time limit, as a chunk does...
   */
  freed = free_apart();
  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(0, 10000);
  unmapped = GC_unmapped_bytes;
  GET_TIME(before);
  CHECK(GC_collect_or_expand(1, 0, FALSE));
  CHECK(GC_unmapped_bytes - unmapped < freed / 2);
  /* Its time limit counts from its own start. */
  since_start = us_since(unmap_start_time);
  CHECK(since_start <= us_since(before));
  /* ...and unmaps more without a time limit. */
  set_time_limit(GC_TIME_UNLIMITED, 0);
  CHECK(GC_collect_or_expand(1, 0, FALSE));
  CHECK(GC_unmapped_bytes - unmapped >= freed / 2);
  set_time_limit(0, 100000);
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  for (i = 0; i < N_APART; i++)
    apart[i] = NULL;
  complete_collection();
  printf("Chunks of the unmapping checked\n");
}
#  endif

int
main(void)
{
  int t, m;
  unsigned long n_stopped;
  GC_bool fine;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    return 0;
  }
  /* Mark in parallel, if available, although no thread is created. */
  GC_start_mark_threads();
  /* Every mark phase has all the markers. */
  GC_set_bytes_per_marker(0);
  check_compaction();
  fine = fine_clock();

  nodes_per_tree = 1;
  for (t = 0; t < DEPTH; t++)
    nodes_per_tree = nodes_per_tree * FANOUT + 1;
  GC_disable();
  for (t = 0; t < N_TREES; t++)
    trees[t] = make_tree((GC_word)t + 1, 0);
  GC_enable();
  for (m = 1; m <= MAX_MARKINGS; m++) {
    mark_with_deadlines(0);
    printf("Marking %d: %lu phases stopped at the deadline, %lu with"
           " a growth of the global mark stack, %lu wrapped compactions,"
           " %lu local mark stacks dropped\n",
           m, n_stopped_phases, n_grown_in_stopped, n_wrapped,
           n_stacks_dropped);
    /* Without the parallel marker, no phase stops at the deadline. */
    if (!GC_get_parallel())
      break;
  }
  if (GC_get_parallel()) {
#  ifdef STEAL_MARK_STACK_RANGES
    /* The ring grows instead. */
    CHECK(0 == n_stacks_dropped);
#  endif
    if (fine) {
      CHECK(n_stopped_phases > 0);
      CHECK(n_grown_in_stopped > 0);
#  ifdef STEAL_MARK_STACK_RANGES
      CHECK(n_wrapped > 0);
#  endif
    } else {
      printf("The clock does not resolve the deadlines, the stopped phases,"
             " growths and wrapped compactions not checked\n");
    }
  }
  check_near_deadline();
  n_stopped = n_stopped_phases;
  mark_with_deadlines(60000);
  printf("Marking with deadlines after 60 s: %lu phases stopped\n",
         n_stopped_phases - n_stopped);
  CHECK(n_stopped_phases == n_stopped);
  GC_gcollect();
  for (t = 0; t < N_TREES; t++)
    CHECK(check_tree(trees[t], (GC_word)t + 1, 0, 0) == nodes_per_tree);

  if (GC_get_parallel()) {
    /*
     * The markings above leave a mark stack smaller than the initial one,
     * while the collector only grows it (with such a mark stack, the push
     * of the dirty blocks would never start).
     */
    LOCK();
    alloc_mark_stack(INITIAL_MARK_STACK_SIZE);
    GC_after_marking();
    UNLOCK();
    GC_enable_incremental();
    if (GC_is_incremental_mode()) {
      /* Every collection marks all the trees. */
      GC_set_full_freq(0);
      GC_set_on_collection_event(on_collection_event);
      check_stopped_marks();
      if (fine) {
        check_abandoned_mark();
        check_step_trigger();
      } else {
        printf("The clock does not resolve 0.1 ms, the checks with that"
               " time limit skipped\n");
      }
      check_final_trigger();
      check_setup_chunks();
#  ifdef GC_PTHREADS
      check_setup_builders();
#  endif
#  if defined(USE_MUNMAP) && !defined(_WIN32)
      if (fine) {
        check_unmap_chunks();
      } else {
        printf("The clock does not resolve 10 us, the unmapping in chunks"
               " not checked\n");
      }
#  endif
      for (t = 0; t < N_TREES; t++)
        CHECK(check_tree(trees[t], (GC_word)t + 1, 0, 0) == nodes_per_tree);
    } else {
      printf("The incremental mode is off, its checks skipped\n");
    }
  }
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The parallel marker deadline is not built, test skipped\n");
  return 0;
}
#endif
