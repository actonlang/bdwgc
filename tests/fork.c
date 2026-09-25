/*
 * Test that the collector is usable in a child process forked while
 * other threads allocate (in the incremental mode, if supported), and
 * while other threads fork.  There are no allocating threads if every
 * allocation acquires a spin lock, and neither allocating nor other
 * forking threads if the atomic operations are emulated by a lock (see
 * `NTHREADS` and `NFORKERS`).  The child process checks that it is
 * registered with its own stack, restarts the parallel markers (if any)
 * and the incremental mode (if it is on in the parent process; not on
 * Darwin yet), allocates and collects.  The test fails if the child
 * process crashes, or hangs (the child process is terminated by a timer
 * in that case), e.g. because it inherits a free-list builder which was
 * in progress at `fork()`.
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

#if defined(GC_PTHREADS) && !defined(GC_WIN32_PTHREADS)  \
    && defined(CAN_HANDLE_FORK) && defined(AO_HAVE_load) \
    && defined(AO_HAVE_store)

#  include <errno.h> /*< for `EAGAIN` */
#  include <pthread.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>

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

/* Number of the allocating threads. */
#  ifndef NTHREADS
#    if defined(USE_SPIN_LOCK) && !defined(THREAD_LOCAL_ALLOC)
/*
 * Every allocation acquires the allocator lock, and the spin lock is not
 * fair: a thread sleeping in `GC_lock()` rarely finds it free, thus the
 * allocating threads would starve the forking ones (for minutes).
 */
#      define NTHREADS 0
#    elif defined(BASE_ATOMIC_OPS_EMULATED)
/*
 * The lock which emulates the atomic operations (in `libatomic_ops`) is
 * not handled at `fork()`: if another thread holds it at `fork()`, then
 * the child process hangs at its first atomic operation.
 */
#      define NTHREADS 0
#    else
#      define NTHREADS 5
#    endif
#  endif

/* Number of the threads which fork (including the main one). */
#  ifndef NFORKERS
#    ifdef BASE_ATOMIC_OPS_EMULATED
/* No other thread should run at `fork()` (see `NTHREADS`). */
#      define NFORKERS 1
#    else
#      define NFORKERS 3
#    endif
#  endif

/* Number of the child processes to fork (one at a time) by each forker. */
#  ifndef NFORKS
#    define NFORKS 50
#  endif

/* Seconds after which a child process is considered hung. */
#  ifndef CHILD_TIMEOUT
#    define CHILD_TIMEOUT 60
#  endif

/* Length of each allocated list. */
#  define LIST_LEN 1000

/* Number of the lists kept live by each allocating thread. */
#  ifndef KEEP_LISTS
#    define KEEP_LISTS 16
#  endif

struct node {
  struct node *next;
  GC_word value;
};

static volatile AO_t stop_allocating = 0;

#  if !defined(NO_INCREMENTAL) && !defined(DARWIN)
/* TODO: Restart the incremental mode in the child process on Darwin too. */
#    define RESTART_INCREMENTAL_IN_CHILD

/* The virtual dirty bits implementation used by the parent process. */
static unsigned parent_vdb;
#  endif

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
  /*
   * The lists kept live by each thread (so that the incremental collection
   * takes more than one step).
   */
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

/*
 * A barrier to make the forkers call `fork()` at about the same time.
 * The number of the parties is set once all the forkers are started.
 */
static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cv = PTHREAD_COND_INITIALIZER;
static int barrier_parties = 0;
static int barrier_arrived = 0;
static unsigned barrier_gen = 0;

/* Release the waiters if all the parties have arrived. */
static void
barrier_release_if_all(void)
{
  if (barrier_arrived == barrier_parties) {
    barrier_arrived = 0;
    barrier_gen++;
    TEST_ASSERT(pthread_cond_broadcast(&barrier_cv) == 0);
  }
}

static void
barrier_set_parties(int n)
{
  TEST_ASSERT(pthread_mutex_lock(&barrier_lock) == 0);
  barrier_parties = n;
  barrier_release_if_all();
  TEST_ASSERT(pthread_mutex_unlock(&barrier_lock) == 0);
}

static void
barrier_wait(void)
{
  unsigned gen;

  TEST_ASSERT(pthread_mutex_lock(&barrier_lock) == 0);
  gen = barrier_gen;
  barrier_arrived++;
  barrier_release_if_all();
  while (gen == barrier_gen) {
    TEST_ASSERT(pthread_cond_wait(&barrier_cv, &barrier_lock) == 0);
  }
  TEST_ASSERT(pthread_mutex_unlock(&barrier_lock) == 0);
}

static void
run_child(const struct GC_stack_base *parent_sb)
{
  struct GC_stack_base sb;
  struct node *l;

  /* Terminate the child process if it hangs. */
  (void)alarm(CHILD_TIMEOUT);
  /*
   * The thread which has forked the process should remain registered
   * (with its stack bottom), even if another thread forked meanwhile.
   */
  TEST_ASSERT(GC_thread_is_registered());
  (void)GC_get_my_stackbottom(&sb);
  TEST_ASSERT(sb.mem_base == parent_sb->mem_base);
  GC_start_mark_threads();
#  ifdef RESTART_INCREMENTAL_IN_CHILD
  if (parent_vdb != GC_VDB_NONE) {
    unsigned vdb;

    /*
     * Turn the incremental mode on again (it is turned off in the child
     * process if `userfaultfd` is used, or if a file of `/proc` cannot be
     * reopened).  The same virtual dirty bits implementation should be
     * used, unless the collector refuses to protect a heap of too many
     * pages, or cannot reopen a file of `/proc` (then the soft-dirty bits
     * are replaced by the `mprotect`-based ones).
     */
    GC_enable_incremental();
    vdb = GC_get_actual_vdb();
    TEST_ASSERT(vdb == parent_vdb || GC_VDB_NONE == vdb
                || (GC_VDB_SOFT == parent_vdb && GC_VDB_MPROTECT == vdb));
  }
#  endif
  l = make_list(LIST_LEN, 7);
  GC_gcollect();
  check_list(l, LIST_LEN, 7);
  l = make_list(LIST_LEN, 11);
  GC_gcollect();
  check_list(l, LIST_LEN, 11);
  exit(0);
}

static void *
fork_many(void *arg)
{
  GC_word seed = (GC_word)(GC_uintptr_t)arg * 100000 + 3;
  struct GC_stack_base sb;
  int i;

  (void)GC_get_my_stackbottom(&sb);
  for (i = 0; i < NFORKS; ++i) {
    struct node *l = make_list(LIST_LEN / 10, seed);
    pid_t pid;
    int wstatus;

    /* Avoid printing the buffered output twice. */
    (void)fflush(stdout);
    barrier_wait();
    /*
     * Make the at-fork prepare handler complete an incremental collection
     * (thus letting the other threads refill their free lists, and fork,
     * meanwhile).
     */
    GC_start_incremental_collection();
    GC_atfork_prepare();
    pid = fork();
    if (0 == pid) {
      GC_atfork_child();
      run_child(&sb);
    }
    GC_atfork_parent();
    if (-1 == pid) {
      fprintf(stderr, "Process fork failed, errno= %d\n", errno);
      exit(69);
    }
    TEST_ASSERT(waitpid(pid, &wstatus, 0) == pid);
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
      fprintf(stderr, "Child process #%d failed, status= 0x%x\n", i, wstatus);
      if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGALRM)
        fprintf(stderr, "Child process was terminated by timeout\n");
      exit(1);
    }
    check_list(l, LIST_LEN / 10, seed);
  }
  /*
   * Do not exit while another forker might be in `fork()`: the exiting
   * thread might hold a lock of the runtime which is not handled at
   * `fork()` (e.g., the Address Sanitizer one of its allocator, while the
   * allocation cache of the thread is released), and the child process
   * would hang once it needs that lock (e.g., to start a marker thread).
   */
  barrier_wait();
  return arg;
}

static int
start_threads(pthread_t *th, int n, void *(*fn)(void *), int first_id)
{
  int i;

  for (i = 0; i < n; ++i) {
    int err = pthread_create(&th[i], NULL, fn,
                             (void *)(GC_uintptr_t)(first_id + i));

    if (err != 0) {
      fprintf(stderr, "Thread #%d creation failed, errno= %d\n", first_id + i,
              err);
      if (i > 0 && EAGAIN == err)
        break;
      exit(69);
    }
  }
  return i;
}

int
main(void)
{
  /* Note: `NTHREADS` might be zero (a zero-size array is not portable). */
  pthread_t th[NTHREADS > 0 ? NTHREADS : 1];
#  if NFORKERS > 1
  pthread_t forkers[NFORKERS - 1];
  int i;
#  endif
  int n, nforkers = 1;

  /* Handle `fork()` either by at-fork handlers or manually. */
  GC_set_handle_fork(-1);
  GC_INIT();
#  ifndef NO_INCREMENTAL
  GC_enable_incremental();
#  endif
#  ifdef RESTART_INCREMENTAL_IN_CHILD
  parent_vdb = GC_get_actual_vdb();
#  endif
  /*
   * Make the collections actually incremental (without a time limit, the
   * world-stopped marking is not interrupted in the parallel marking mode).
   */
  GC_set_time_limit(1);

  n = start_threads(th, NTHREADS, allocate, 0);
#  if NFORKERS > 1
  nforkers += start_threads(forkers, NFORKERS - 1, fork_many, NTHREADS + 1);
#  endif
  barrier_set_parties(nforkers);
  (void)fork_many((void *)(GC_uintptr_t)NTHREADS);
#  if NFORKERS > 1
  for (i = nforkers - 1; i-- > 0;) {
    TEST_ASSERT(pthread_join(forkers[i], NULL) == 0);
  }
#  endif

  AO_store(&stop_allocating, 1);
  while (n-- > 0) {
    TEST_ASSERT(pthread_join(th[n], NULL) == 0);
  }
  printf("SUCCEEDED (%d child processes, incremental: %s)\n",
         nforkers * NFORKS, GC_is_incremental_mode() ? "yes" : "no");
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
