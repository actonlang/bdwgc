/*
 * Test the marking of collections that span several world-stopped marks:
 * threads keep forests of wide trees, replace trees and subtrees while the
 * collections are in progress (storing new objects into old ones), and
 * check every node of their forests.  A node freed while reachable is
 * reused by another allocation, thus its key or check word is wrong.
 * Each thread replaces trees in `N_ROUNDS` rounds and then goes on until
 * a marking completed (at most `MAX_ROUNDS`), since the steps that
 * a collection takes depend on the load of the machine.  Once a thread
 * saw a completed marking, it checks its forest once more before it stops.
 * Unless `NO_INCREMENTAL` is defined, the test turns the incremental mode
 * on with a time limit of 0.1 ms (by `GC_ENABLE_INCREMENTAL` and
 * `GC_PAUSE_TIME_TARGET` environment variables).  With the parallel
 * marker, the test checks that the time limit was parsed, that some
 * collections were marked in several world-stopped marks, and that the
 * world was stopped for marking steps between them too.  It checks the
 * same in the manual VDB mode with one marker (`GC_MARKERS=1`), whose
 * steps run on one thread, in the builds that parse the fraction of the
 * time limit (those with the parallel marker).  At last, it starts
 * a collection and waits for it (`GC_gcollect()`), which marks in steps
 * without allocation.  Then, with a stop function which always returns
 * true, every world-stopped mark with the time limit is abandoned before it
 * marks anything (in the manual VDB mode, once it pushed the roots), and
 * a collection must still complete, marked only by steps, each of which
 * calls `GC_mark_some()` once, and by the final mark without the time limit.
 *
 * If `NO_CLOCK` is defined, the collector has no time limit, and the test
 * checks only the forests.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifndef GC_THREADS
#  define GC_THREADS
#endif

#include "gc.h"

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

#define N_THREADS 4

/* The trees of each thread. */
#define N_TREES 16

/* The children of a node, and the levels of a tree below its root. */
#define FANOUT 8
#define DEPTH 4

/* The least and the most trees (or subtrees) that each thread builds. */
#define N_ROUNDS 300
#define MAX_ROUNDS (30 * N_ROUNDS)

/* A thread checks its forest after this many rounds. */
#define CHECK_EVERY 25

/* The nodes of a tree, set by `main()`. */
static unsigned long nodes_per_tree;

struct node {
  GC_word key;
  GC_word check;
  struct node *child[FANOUT];
};

static GC_word
check_of(GC_word key, int level)
{
  return key * (GC_word)2654435761U + (GC_word)level;
}

/*
 * The world stops, the world-stopped marks (`GC_EVENT_MARK_START`) and the
 * completed markings of the collections.  Updated by
 * `on_collection_event()`, which the collector calls holding the allocator
 * lock, and read after the threads are joined (`n_marks` also holding the
 * lock, by `marks_seen()`).
 */
static unsigned long n_stops;
static unsigned long n_stopped_marks;
static unsigned long n_marks;

static void GC_CALLBACK
on_collection_event(GC_EventType e)
{
  switch (e) {
  case GC_EVENT_POST_STOP_WORLD:
    n_stops++;
    break;
  case GC_EVENT_MARK_START:
    n_stopped_marks++;
    break;
  case GC_EVENT_MARK_END:
    n_marks++;
    break;
  default:
    break;
  }
}

static void *GC_CALLBACK
read_marks(void *cd)
{
  *(unsigned long *)cd = n_marks;
  return NULL;
}

/* The completed markings so far. */
static unsigned long
marks_seen(void)
{
  unsigned long n;

  (void)GC_call_with_alloc_lock(read_marks, &n);
  return n;
}

static GC_ATTR_NORETURN void
fail(const char *what, GC_word key, int level)
{
  fprintf(stderr, "Wrong %s at level %d (key %lu)\n", what, level,
          (unsigned long)key);
  exit(1);
}

static struct node *
new_node(GC_word key, int level)
{
  struct node *n = (struct node *)GC_MALLOC(sizeof(struct node));

  if (NULL == n) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  n->key = key;
  n->check = check_of(key, level);
  return n;
}

/* Build a tree of the levels from `level` to `DEPTH`. */
static struct node *
make_tree(GC_word key, int level)
{
  struct node *n = new_node(key, level);

  if (level < DEPTH) {
    int i;

    for (i = 0; i < FANOUT; i++) {
      struct node *c = make_tree(key * FANOUT + (GC_word)i + 1, level + 1);

      n->child[i] = c;
      GC_END_STUBBORN_CHANGE(n);
    }
  }
  return n;
}

/* Check every node of the tree, and return the number of its nodes. */
static unsigned long
check_tree(const struct node *n, GC_word key, int level)
{
  unsigned long count = 1;
  int i;

  if (NULL == n)
    fail("missing node", key, level);
  if (n->key != key)
    fail("key", key, level);
  if (n->check != check_of(key, level))
    fail("check word", key, level);
  for (i = 0; i < FANOUT; i++) {
    if (level < DEPTH) {
      count
          += check_tree(n->child[i], key * FANOUT + (GC_word)i + 1, level + 1);
    } else if (n->child[i] != NULL) {
      fail("leaf", key, level);
    }
  }
  return count;
}

#ifdef GC_PTHREADS
static void *
run(void *arg)
#else
static DWORD WINAPI
run(LPVOID arg)
#endif
{
  unsigned seed = (unsigned)(GC_uintptr_t)arg;
  struct node **forest;
  int round, i;
  int seen = 0;

  forest = (struct node **)GC_MALLOC(N_TREES * sizeof(struct node *));
  if (NULL == forest) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  for (i = 0; i < N_TREES; i++) {
    forest[i] = make_tree((GC_word)i + 1, 0);
    GC_END_STUBBORN_CHANGE(forest);
  }
  for (round = 1;; round++) {
    seed = seed * 1103515245U + 12345U;
    i = (int)((seed >> 8) % N_TREES);
    if ((round & 1) != 0) {
      /* Replace a whole tree; the forest is an old object. */
      forest[i] = make_tree((GC_word)i + 1, 0);
      GC_END_STUBBORN_CHANGE(forest);
    } else {
      /*
       * Replace a subtree below a random path; the parent of the new
       * subtree is an old object (or a new one, if the path crosses
       * a replaced subtree).
       */
      GC_word key = (GC_word)i + 1;
      struct node *n = forest[i];
      int level, c;

      for (level = 0;; level++) {
        seed = seed * 1103515245U + 12345U;
        c = (int)((seed >> 8) % FANOUT);
        if (level + 1 >= DEPTH || ((seed >> 20) & 3) == 0)
          break;
        key = key * FANOUT + (GC_word)c + 1;
        n = n->child[c];
      }
      n->child[c] = make_tree(key * FANOUT + (GC_word)c + 1, level + 1);
      GC_END_STUBBORN_CHANGE(n);
    }
    if (round % CHECK_EVERY == 0) {
      unsigned long count = 0;

      for (i = 0; i < N_TREES; i++)
        count += check_tree(forest[i], (GC_word)i + 1, 0);
      if (count != N_TREES * nodes_per_tree)
        fail("node count", 0, 0);
      /* Check the forest once more after a completed marking was seen. */
      if (round >= MAX_ROUNDS || (round >= N_ROUNDS && seen))
        break;
      seen = marks_seen() > 0;
    }
  }
#ifdef GC_PTHREADS
  return arg;
#else
  return 0;
#endif
}

#ifndef NO_CLOCK
static int GC_CALLBACK
always_stop(void)
{
  return 1;
}

/*
 * Start a collection and complete it with the stop function always
 * returning true: it is marked only by steps and by the final mark without
 * the time limit.
 */
static void
check_step_progress(void)
{
  GC_stop_func stop_func = GC_get_stop_func();
  GC_word gc_no = GC_get_gc_no();
  long n;

  GC_set_stop_func(always_stop);
  GC_start_incremental_collection();
  for (n = 0; n < 100000L && GC_collect_a_little(); n++) {
    /* Empty. */
  }
  GC_set_stop_func(stop_func);
  if (GC_get_gc_no() == gc_no) {
    fprintf(stderr, "No collection completed in steps\n");
    exit(1);
  }
}
#endif

int
main(void)
{
  int i;
#ifndef NO_CLOCK
  unsigned long n_marks_run;
  struct GC_timeval_s tv;
  int parsed;
#endif
#ifdef GC_PTHREADS
  pthread_t th[N_THREADS];
#else
  HANDLE th[N_THREADS];
#endif

#ifndef NO_INCREMENTAL
#  ifdef _WIN32
  (void)_putenv("GC_ENABLE_INCREMENTAL=1");
  (void)_putenv("GC_PAUSE_TIME_TARGET=0.1");
#  else
  (void)setenv("GC_ENABLE_INCREMENTAL", "1", 1);
  (void)setenv("GC_PAUSE_TIME_TARGET", "0.1", 1);
#  endif
#endif
  GC_INIT();
  GC_set_on_collection_event(on_collection_event);
  nodes_per_tree = 1;
  for (i = 0; i < DEPTH; i++)
    nodes_per_tree = nodes_per_tree * FANOUT + 1;
  for (i = 0; i < N_THREADS; i++) {
#ifdef GC_PTHREADS
    int err = pthread_create(&th[i], NULL, run, (void *)(GC_uintptr_t)(i + 1));

    if (err != 0) {
      fprintf(stderr, "Thread creation failed, errno= %d\n", err);
      exit(69);
    }
#else
    DWORD id;

    th[i] = CreateThread(NULL, 0, run, (LPVOID)(GC_uintptr_t)(i + 1), 0, &id);
    if (NULL == th[i]) {
      fprintf(stderr, "Thread creation failed, errcode= %d\n",
              (int)GetLastError());
      exit(69);
    }
#endif
  }
  for (i = 0; i < N_THREADS; i++) {
#ifdef GC_PTHREADS
    int err = pthread_join(th[i], NULL);

    if (err != 0) {
      fprintf(stderr, "Thread join failed, errno= %d\n", err);
      exit(69);
    }
#else
    if (WaitForSingleObject(th[i], INFINITE) != WAIT_OBJECT_0) {
      fprintf(stderr, "Thread join failed, errcode= %d\n",
              (int)GetLastError());
      exit(69);
    }
    CloseHandle(th[i]);
#endif
  }
#ifndef NO_CLOCK
  n_marks_run = n_marks;
#endif
  GC_start_incremental_collection();
  GC_gcollect();
#ifndef NO_CLOCK
  tv = GC_get_time_limit_tv();
  parsed = 0 == tv.tv_ms && 100000UL == tv.tv_nsec;
  if (GC_is_incremental_mode()
      && (GC_get_parallel()
          || (parsed && GC_get_actual_vdb() == GC_VDB_MANUAL))) {
    if (!parsed) {
      fprintf(stderr, "Wrong time limit: %lu ms %lu ns\n", tv.tv_ms,
              tv.tv_nsec);
      exit(1);
    }
    if (0 == n_marks_run) {
      fprintf(stderr, "No collection completed while the threads ran\n");
      exit(1);
    }
    if (n_stopped_marks <= n_marks) {
      fprintf(stderr, "No collection spans several world-stopped marks\n");
      exit(1);
    }
    if (n_stops <= n_stopped_marks) {
      fprintf(stderr, "No marking step\n");
      exit(1);
    }
    check_step_progress();
  }
#endif
  printf("SUCCEEDED (%lu collections, %lu world stops, %lu world-stopped"
         " marks, %lu completed)\n",
         (unsigned long)GC_get_gc_no(), n_stops, n_stopped_marks, n_marks);
  return 0;
}
