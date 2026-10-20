/*
 * Test that generational collection with the `userfaultfd` virtual dirty
 * bits (in the asynchronous or the synchronous mode, whichever the kernel
 * supports) sees every write to an old object.  The old objects are of
 * three shapes: pointer arrays of many pages, objects bigger than a page
 * (written at their last word) and two-word objects, allocated before
 * the first collection and after it.  In each round, the test stores
 * a pointer to a new object (a child) into some pages of them; a child is
 * reachable only through such a store.  Then a partial collection follows.
 * It marks from an old object only if a write to its block (to any of its
 * blocks, if the object is bigger than half a block) has been recorded,
 * thus if the write is missed, the child is not marked and is freed.  The
 * test detects that through a disappearing link to each child (cleared
 * when the child is collected), and through the contents of each child
 * after objects of its size have been allocated again (the memory of
 * a freed child is reused, and its first word links it into a free list).
 *
 * A recorded write to another page of the same block (or object) would
 * hide a missed one, thus every round except each fourth one writes one
 * page of each block (of each object bigger than half a block, of each
 * page bigger than a block); each fourth round writes all pages.  The page
 * written changes every two rounds, thus a page is written again after the
 * collection which recorded its previous write has protected it again, and
 * a page is written after collections which found it clean.  In every
 * third round of the first 12, several threads store the children while
 * the main thread runs partial collections, thus the world is stopped
 * while the stores fault.
 *
 * In rounds 12 and 13, a system call stores each pointer: `read()` from
 * a pipe into the old object.  In the asynchronous mode, the kernel
 * removes the protection of a page at such a write as at any other one.
 * In the synchronous mode, the call fails with `EFAULT` if the page is
 * protected (the collector opens `userfaultfd` with `UFFD_USER_MODE_ONLY`
 * where the kernel has it), and then the test stores the pointer itself.
 * In round 14, the main thread forks a child process (with the fork
 * handling of the collector on).  It stores into the old objects
 * allocated before the first collection before `fork()`, and into those
 * allocated after it while the child process runs (on pages shared
 * copy-on-write with the child process).  Meanwhile, the child process
 * turns the incremental mode on again (the collector turns it off in the
 * child process), stores into its copy of the former objects and checks
 * them after a partial collection of its own.  The parent process does
 * not write these objects after `fork()`, thus its writes cannot hide
 * a write which the child process misses (e.g. if the latter scanned the
 * page table of the parent process).  The partial collection of the
 * parent process which follows must find the children stored before and
 * after `fork()`.
 *
 * A collection which marks from the roots would find every child without
 * the dirty bits, thus the test fails if a partial collection turns out
 * to be a full one.  Skipped unless the collector uses the `userfaultfd`
 * virtual dirty bits, and if `NO_INCREMENTAL` is defined.
 */

#ifdef HAVE_CONFIG_H
/* For `GC_THREADS` and `HANDLE_FORK` macros. */
#  include "config.h"
#endif

#undef GC_NO_THREAD_REDIRECTS
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(UFFDWP_VDB) && defined(GC_PTHREADS) && !defined(NO_INCREMENTAL)

#  include <errno.h>
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>

#  ifdef CAN_HANDLE_FORK
#    include <sys/types.h>
#    include <sys/wait.h>
#  endif

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

/* The minimum page size, the distance of the stores to a pointer array. */
#  define STORE_STEP 4096

/* The pointer arrays, one store per `STORE_STEP` bytes. */
#  define N_BIG 4
#  define BIG_PAGES 128
#  define BIG_BYTES ((size_t)BIG_PAGES * STORE_STEP)

/* The objects bigger than a page. */
#  define N_MEDIUM 64
#  define MEDIUM_BYTES 6000

/* The two-word objects. */
#  define N_SMALL 8192

/* The sets of the old objects: allocated before and after a collection. */
#  define N_SETS 2

#  define SLOTS_PER_SET (N_BIG * BIG_PAGES + N_MEDIUM + N_SMALL)
#  define N_SLOTS (N_SETS * SLOTS_PER_SET)

#  define CHILD_WORDS 4

#  define MAGIC ((GC_word)0x5a5a1234)

#  define N_ROUNDS 16

/* The rounds which store through a system call, and the one which forks. */
#  define FIRST_SYSCALL_ROUND 12
#  define N_SYSCALL_ROUNDS 2
#  define FORK_ROUND 14

/* The number of the threads storing the children in a concurrent round. */
#  define N_WRITERS 4

/* The old objects (the arrays are the roots). */
static void **big[N_SETS][N_BIG];
static void **medium[N_SETS][N_MEDIUM];
static void **small[N_SETS][N_SMALL];

/* The location of the pointer to each child, inside an old object. */
static void **slots[N_SLOTS];

/* The hidden pointer to each child, cleared if the child is collected. */
static GC_hidden_pointer links[N_SLOTS];

/* The round which stored each child, or -1. */
static int stored_round[N_SLOTS];

/* The full collections started; updated with the allocator lock held. */
static volatile int n_full_collections;

/*
 * Whether the fork round has run a child process, whether that failed,
 * and whether this is it.
 */
static int forked, child_failed;
static int in_child;

static void GC_CALLBACK
on_full_collection(void)
{
  n_full_collections++;
}

static void
alloc_set(int set)
{
  int i;

  for (i = 0; i < N_BIG; i++) {
    big[set][i] = (void **)GC_MALLOC(BIG_BYTES);
    CHECK_OUT_OF_MEMORY(big[set][i]);
  }
  for (i = 0; i < N_MEDIUM; i++) {
    medium[set][i] = (void **)GC_MALLOC(MEDIUM_BYTES);
    CHECK_OUT_OF_MEMORY(medium[set][i]);
  }
  for (i = 0; i < N_SMALL; i++) {
    small[set][i] = (void **)GC_MALLOC(2 * sizeof(void *));
    CHECK_OUT_OF_MEMORY(small[set][i]);
  }
}

static void
init_slots(void)
{
  int set, i, n = 0;

  for (set = 0; set < N_SETS; set++) {
    for (i = 0; i < N_BIG; i++) {
      int page;

      for (page = 0; page < BIG_PAGES; page++) {
        /* A different word of each page. */
        size_t ofs = (size_t)page * STORE_STEP
                     + (size_t)(page * 7 % (STORE_STEP / sizeof(void *)))
                           * sizeof(void *);

        slots[n++] = &big[set][i][ofs / sizeof(void *)];
      }
    }
    for (i = 0; i < N_MEDIUM; i++)
      slots[n++] = &medium[set][i][MEDIUM_BYTES / sizeof(void *) - 1];
    for (i = 0; i < N_SMALL; i++)
      slots[n++] = &small[set][i][0];
  }
  TEST_ASSERT(N_SLOTS == n);
  for (n = 0; n < N_SLOTS; n++)
    stored_round[n] = -1;
}

/* The page size of the system. */
static GC_uintptr_t page_size;

/* The old object holding slot `n` (in the order of `init_slots()`). */
static void *
slot_object(int n)
{
  int set = n / SLOTS_PER_SET;
  int i = n % SLOTS_PER_SET;

  if (i < N_BIG * BIG_PAGES)
    return big[set][i / BIG_PAGES];
  i -= N_BIG * BIG_PAGES;
  if (i < N_MEDIUM)
    return medium[set][i];
  return small[set][i - N_MEDIUM];
}

/*
 * Whether slot `n` is written in round `round`.  Each fourth round writes
 * all slots.  The other rounds write the slots on one page of the unit of
 * each slot: the block of its old object, or the object if it is bigger
 * than half a block, or the page if it is bigger than these.  The page of
 * a unit is the same in rounds 4k + 1 and 4k + 2, and changes in the other
 * rounds.
 */
static int
is_stored_in_round(int round, int n)
{
  GC_uintptr_t addr = (GC_uintptr_t)slots[n];
  GC_uintptr_t start, end, first_page, n_pages;
  void *obj;
  size_t sz;

  if (round % 4 == 0)
    return 1;
  obj = slot_object(n);
  sz = GC_size(obj);
  if (sz > MAXOBJBYTES) {
    start = (GC_uintptr_t)obj;
    end = start + sz;
  } else {
    start = addr & ~(GC_uintptr_t)(HBLKSIZE - 1);
    end = start + HBLKSIZE;
  }
  first_page = start / page_size;
  n_pages = (end - 1) / page_size - first_page + 1;
  return addr / page_size - first_page
         == ((GC_uintptr_t)((round + 1) / 2) * 5 + first_page) % n_pages;
}

/* Allocate the child of slot `n` in round `round`. */
static void *
new_child(int round, int n)
{
  GC_word *q = (GC_word *)GC_MALLOC(CHILD_WORDS * sizeof(GC_word));

  CHECK_OUT_OF_MEMORY(q);
  q[0] = MAGIC ^ (GC_word)n;
  q[CHILD_WORDS - 1] = MAGIC + (GC_word)round;
  links[n] = GC_HIDE_POINTER(q);
#  ifndef GC_NO_FINALIZATION
  {
    /* Moves the link of the previous child, if any, to this one. */
    int res = GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&links[n], q);

    TEST_ASSERT(GC_SUCCESS == res || GC_DUPLICATE == res);
  }
#  endif
  stored_round[n] = round;
  return q;
}

static void
store_child(int round, int n)
{
  /* This is the only reference to the child. */
  *slots[n] = new_child(round, n);
}

/* Store the children of the round into the old objects of set `set`. */
static void
store_children_of_set(int round, int set)
{
  int n;

  for (n = set * SLOTS_PER_SET; n < (set + 1) * SLOTS_PER_SET; n++) {
    if (is_stored_in_round(round, n))
      store_child(round, n);
  }
}

static void
store_children(int round)
{
  int set;

  for (set = 0; set < N_SETS; set++)
    store_children_of_set(round, set);
}

/* The pipe through which `read()` stores the pointers to the children. */
static int store_pipe[2];

/* The stores by `read()`, and those of them which failed with `EFAULT`. */
static int n_syscall_stores, n_syscall_faults;

/*
 * Store the children of the round with `read()`.  If it fails with
 * `EFAULT`, then the pointer stays in the pipe, and is stored directly.
 */
static void
store_children_by_syscall(int round)
{
  int n;

  for (n = 0; n < N_SLOTS; n++) {
    void *q;
    ssize_t res;

    if (!is_stored_in_round(round, n))
      continue;
    q = new_child(round, n);
    TEST_ASSERT(write(store_pipe[1], &q, sizeof(q)) == (ssize_t)sizeof(q));
    n_syscall_stores++;
    res = read(store_pipe[0], slots[n], sizeof(q));
    if (-1 == res && EFAULT == errno) {
      void *p;

      n_syscall_faults++;
      TEST_ASSERT(read(store_pipe[0], &p, sizeof(p)) == (ssize_t)sizeof(p));
      *slots[n] = p;
    } else {
      TEST_ASSERT(res == (ssize_t)sizeof(q));
    }
    TEST_ASSERT(*slots[n] == q);
  }
}

static int n_partial_collections;

static void
collect_partially(void)
{
  GC_word gc_no = GC_get_gc_no();
  int n_full = n_full_collections;

  GC_start_incremental_collection();
  while (GC_collect_a_little()) {
    /* Empty. */
  }
  TEST_ASSERT(GC_get_gc_no() > gc_no);
  if (n_full_collections != n_full) {
    fprintf(stderr, "A full collection ran instead of a partial one%s\n",
            in_child ? " (in child process)" : "");
    exit(1);
  }
  n_partial_collections++;
}

/* The slots visited by the writers between the collections. */
#  define SLOTS_PER_COLLECTION (N_SLOTS / 16)

static pthread_mutex_t writers_ml = PTHREAD_MUTEX_INITIALIZER;
static int writers_round;

/* The slots visited by the writers, and the writers done; under the lock. */
static int n_visited;
static int n_writers_done;

static void
add_visited(int n, int done)
{
  TEST_ASSERT(pthread_mutex_lock(&writers_ml) == 0);
  n_visited += n;
  n_writers_done += done;
  TEST_ASSERT(pthread_mutex_unlock(&writers_ml) == 0);
}

static void *
writer(void *arg)
{
  int n, k = 0;

  for (n = (int)(GC_uintptr_t)arg; n < N_SLOTS; n += N_WRITERS) {
    if (is_stored_in_round(writers_round, n))
      store_child(writers_round, n);
    if (++k == 256) {
      add_visited(k, 0);
      k = 0;
    }
  }
  add_visited(k, 1);
  return NULL;
}

/*
 * Store the children of the round with several threads, with a partial
 * collection after every `SLOTS_PER_COLLECTION` slots they visit.
 */
static void
store_children_concurrently(int round)
{
  pthread_t th[N_WRITERS];
  int i, next = SLOTS_PER_COLLECTION;

  writers_round = round;
  n_visited = 0;
  n_writers_done = 0;
  for (i = 0; i < N_WRITERS; i++) {
    int err = pthread_create(&th[i], NULL, writer, (void *)(GC_uintptr_t)i);

    if (err != 0) {
      fprintf(stderr, "Thread creation failed, errno= %d\n", err);
      exit(1);
    }
  }
  for (;;) {
    int visited, done;

    TEST_ASSERT(pthread_mutex_lock(&writers_ml) == 0);
    visited = n_visited;
    done = n_writers_done;
    TEST_ASSERT(pthread_mutex_unlock(&writers_ml) == 0);
    if (N_WRITERS == done)
      break;
    if (visited >= next) {
      collect_partially();
      next = visited + SLOTS_PER_COLLECTION;
    } else {
      (void)sched_yield();
    }
  }
  for (i = 0; i < N_WRITERS; i++)
    TEST_ASSERT(pthread_join(th[i], NULL) == 0);
}

/*
 * Allocate unreachable objects of the size of the children.  This also
 * uses up the objects of the thread-local free lists, which a collection
 * marks (thus a child allocated from them would stay alive without the
 * dirty bits), and the children of the next round are allocated unmarked.
 */
static void
reuse_free_memory(void)
{
  int n;

  for (n = 0; n < N_SLOTS; n++) {
    GC_word *p = (GC_word *)GC_MALLOC(CHILD_WORDS * sizeof(GC_word));

    CHECK_OUT_OF_MEMORY(p);
    p[0] = ~MAGIC;
    p[CHILD_WORDS - 1] = ~MAGIC;
  }
}

static void
check_children(int round)
{
  int n, n_lost = 0, n_changed = 0;

  for (n = 0; n < N_SLOTS; n++) {
    const GC_word *q;

    if (stored_round[n] < 0)
      continue;
    if (0 == links[n]) {
      n_lost++;
      continue;
    }
    q = (const GC_word *)GC_REVEAL_POINTER(links[n]);
    if ((const GC_word *)(*slots[n]) != q || q[0] != (MAGIC ^ (GC_word)n)
        || q[CHILD_WORDS - 1] != MAGIC + (GC_word)stored_round[n])
      n_changed++;
  }
  if (n_lost > 0 || n_changed > 0) {
    fprintf(stderr,
            "Round %d%s: of %d children, %d collected and %d changed while"
            " reachable\n",
            round, in_child ? " (child process)" : "", (int)N_SLOTS, n_lost,
            n_changed);
    exit(1);
  }
}

#  ifdef CAN_HANDLE_FORK
/* Seconds after which the child process is considered hung. */
#    define CHILD_TIMEOUT 60

/*
 * Run the child process of the fork round: turn the incremental mode on
 * again, store the children of the round into the old objects allocated
 * before the first collection, check them after a partial collection, and
 * exit once the parent process has stored its children (and closed the
 * pipe `rfd` reads).
 */
static void
run_child(int round, int rfd)
{
  char c;

  in_child = 1;
  /* Terminate the child process if it hangs. */
  (void)alarm(CHILD_TIMEOUT);
  GC_start_mark_threads();
  GC_enable_incremental();
  TEST_ASSERT(GC_get_actual_vdb() == GC_VDB_UFFDWP);
  /*
   * Use up the objects of the thread-local free lists, which the
   * collection that turned the incremental mode on has marked.
   */
  reuse_free_memory();
  store_children_of_set(round, 0);
  collect_partially();
  reuse_free_memory();
  check_children(round);
  TEST_ASSERT(read(rfd, &c, 1) >= 0);
  exit(0);
}

/*
 * Store the children of the round into the old objects allocated before
 * the first collection, fork a child process, and store the other ones
 * while the child process runs (and shares the pages of the parent
 * process copy-on-write).
 */
static void
store_children_around_fork(int round)
{
  int fds[2], wstatus;
  pid_t pid;

  store_children_of_set(round, 0);
  TEST_ASSERT(pipe(fds) == 0);
  /* Avoid printing the buffered output twice. */
  (void)fflush(stdout);
  GC_atfork_prepare();
  pid = fork();
  if (0 == pid) {
    GC_atfork_child();
    TEST_ASSERT(close(fds[1]) == 0);
    run_child(round, fds[0]);
  }
  GC_atfork_parent();
  if (-1 == pid) {
    fprintf(stderr, "Process fork failed, errno= %d\n", errno);
    exit(69);
  }
  TEST_ASSERT(close(fds[0]) == 0);
  store_children_of_set(round, 1);
  /* Let the child process exit. */
  TEST_ASSERT(close(fds[1]) == 0);
  TEST_ASSERT(waitpid(pid, &wstatus, 0) == pid);
  forked = 1;
  if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
    /* The children of the parent process are checked first. */
    fprintf(stderr, "Child process failed, status= 0x%x\n", wstatus);
    child_failed = 1;
  }
}
#  endif

int
main(void)
{
  int round;

#  ifdef CAN_HANDLE_FORK
  /* Handle `fork()` either by at-fork handlers or manually. */
  GC_set_handle_fork(-1);
#  endif
  GC_INIT();
  page_size = (GC_uintptr_t)sysconf(_SC_PAGESIZE);
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    printf("test skipped\n");
    return 0;
  }
  /* Only the collections below are done. */
  GC_set_disable_automatic_collection(1);
  /* Generational collection: each collection runs to its end at once. */
  GC_set_time_limit(GC_TIME_UNLIMITED);
  GC_set_full_freq(1000);
  /*
   * A partial collection is followed by a full one once the heap in use
   * has grown by more than the minimum allocation between collections
   * since the last full collection.
   */
  GC_set_min_bytes_allocd((size_t)1 << 30);
  GC_set_start_callback(on_full_collection);
  GC_enable_incremental();
  if (GC_get_actual_vdb() != GC_VDB_UFFDWP) {
    printf("test skipped (no userfaultfd dirty bits)\n");
    return 0;
  }
  TEST_ASSERT(pipe(store_pipe) == 0);

  alloc_set(0);
  /* Make the first set old. */
  GC_gcollect();
  /* The second set becomes old at the collection of the first round. */
  alloc_set(1);
  init_slots();

  for (round = 0; round < N_ROUNDS; round++) {
    if (round >= FIRST_SYSCALL_ROUND
        && round < FIRST_SYSCALL_ROUND + N_SYSCALL_ROUNDS) {
      store_children_by_syscall(round);
    } else if (FORK_ROUND == round) {
#  ifdef CAN_HANDLE_FORK
      store_children_around_fork(round);
#  else
      store_children(round);
#  endif
    } else if (round % 3 == 2) {
      store_children_concurrently(round);
    } else {
      store_children(round);
    }
    collect_partially();
    reuse_free_memory();
    check_children(round);
    if (child_failed)
      exit(1);
  }
  GC_gcollect();
  check_children(N_ROUNDS);
  printf("SUCCEEDED (%d partial collections, %d of %d system-call stores"
         " failed with EFAULT, forked: %s, heap size: %lu)\n",
         n_partial_collections, n_syscall_faults, n_syscall_stores,
         forked ? "yes" : "no", (unsigned long)GC_get_heap_size());
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
