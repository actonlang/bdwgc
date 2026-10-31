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
 * The write-barrier checker of the manual VDB mode (`GC_WBAR_VERIFY`) and
 * the collector's own dirtying in that mode.  Run without arguments, the
 * cases marked clean below run, each of which must report no error: stores
 * with a barrier (`barrier`, `threads`), a store whose barrier runs after
 * a collection found its target on a stack (`window`), a marked object
 * freed onto a free list, whose link is no store (`free-kept`), and
 * objects the collector itself must dirty or rescan: objects from
 * a thread-local free list kept over a collection (`tlfs`), an explicitly
 * freed object which is reused (`free`), an object initialized across
 * a collection (`stack`), an uncollectable object (`uncoll`) and an object
 * of a kind set always rescanned (`kind`).  With an argument, one case runs;
 * the cases `lost`, `missed`, `missed-next` and `kind-unflagged` store
 * without a barrier and must be reported (as lost, as missed, as missed at
 * the next collection and as lost), after which the checker makes the
 * process exit with status 70.  So must the case `lost-exit`, in which
 * `exit()` is called while the report is printed.  Skipped unless the
 * manual VDB mode is built, and if `NO_INCREMENTAL` is defined.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* For `NORMAL`, `GC_generic_malloc()` and the checker's totals. */
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if !defined(NO_MANUAL_VDB) && !defined(NO_INCREMENTAL)

#  include <string.h>

#  ifdef GC_PTHREADS
#    include <pthread.h>
#  endif

#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define MAGIC 0x5eed1234L

struct young {
  long magic;
  void *pad[4];
};

/*
 * The old objects are of a size of their own, so that their blocks hold
 * no other objects of the test.
 */
struct old {
  struct young *p;
  void *pad[123];
  struct young *last;
};

static void *volatile g_old;
static void *volatile g_young;
static void *volatile g_young_last;
static void *volatile g_obj;

static GC_word
gc_no(void)
{
  return GC_get_gc_no();
}

/* Allocate pointer-free objects until a collection completes. */
static GC_ATTR_NOINLINE void
collect(void)
{
  GC_word n = gc_no();
  int i;

  for (i = 0; gc_no() == n; i++) {
    CHECK(i < 10000000);
    CHECK(GC_MALLOC_ATOMIC(4096) != NULL);
  }
}

/*
 * The same, but the collection is a minor one, which the checker checks:
 * the collector would make it a full one if the heap grew since the latest
 * full collection.
 */
static void
collect_minor(void)
{
  GC_alloc_lock();
  GC_need_full_gc = FALSE;
  GC_alloc_unlock();
  collect();
}

/* Overwrite the stack below the caller, to drop stale pointers. */
static GC_ATTR_NOINLINE void
clear_stack(void)
{
  volatile char buf[16384];

  memset((char *)buf, 0, sizeof(buf));
  GC_noop1((GC_word)(GC_uintptr_t)buf);
}

/*
 * The young objects the cases store are allocated outside of the
 * thread-local free lists, thus unmarked: the objects of a thread-local
 * free list are marked by every root push, and a store of a pointer to
 * a marked object needs no barrier.
 */
static GC_ATTR_NOINLINE struct young *
new_young(void)
{
  struct young *y
      = (struct young *)GC_generic_malloc(sizeof(struct young), NORMAL);

  CHECK(y != NULL);
  y->magic = MAGIC;
  return y;
}

/* An old object: allocated outside of the thread-local free lists. */
static GC_ATTR_NOINLINE struct old *
new_old(void)
{
  struct old *o = (struct old *)GC_generic_malloc(sizeof(struct old), NORMAL);

  CHECK(o != NULL);
  memset(o, 0, sizeof(*o));
  return o;
}

static GC_ATTR_NOINLINE void
make_old(void)
{
  g_old = new_old();
}

static GC_ATTR_NOINLINE void
store_young(int barrier, int keep)
{
  struct old *o = (struct old *)g_old;
  struct young *y = new_young();

  o->p = y;
  if (barrier) {
    GC_END_STUBBORN_CHANGE(&o->p);
    GC_reachable_here(y);
  } else {
    /*
     * The checker leaves the target of each word it records in stale
     * words on the stack of the collecting thread, where the stack scan
     * of the same collection may find it: then the target looks held by
     * a stack, and is deferred to the next collection.  A second store
     * without a barrier, into the last word, whose target is kept like
     * the first one, is checked after it and leaves its own target there
     * instead.
     */
    o->last = new_young();
  }
  if (keep) {
    g_young = y;
    g_young_last = o->last;
  }
}

static GC_ATTR_NOINLINE int
old_intact(void)
{
  struct old *o = (struct old *)g_old;

  return o->p != NULL && o->p->magic == MAGIC;
}

/* The totals before the current case. */
static struct GC_wbar_totals_s base;

/* The totals of the current case. */
static void
totals(struct GC_wbar_totals_s *t)
{
  GC_get_wbar_verify_totals(t);
  t->lost -= base.lost;
  t->missed -= base.missed;
  t->window -= base.window;
  t->checks -= base.checks;
}

static GC_word
errors(void)
{
  struct GC_wbar_totals_s t;

  totals(&t);
  return t.lost + t.missed;
}

static GC_word
windows(void)
{
  struct GC_wbar_totals_s t;

  totals(&t);
  return t.window;
}

static void
report(const char *name)
{
  struct GC_wbar_totals_s t;

  totals(&t);
  printf("%s: checks=%lu lost=%lu missed=%lu window=%lu\n", name,
         (unsigned long)t.checks, (unsigned long)t.lost,
         (unsigned long)t.missed, (unsigned long)t.window);
}

/* Make an old object, marked by two minor collections. */
static void
setup_old(void)
{
  make_old();
  clear_stack();
  collect();
  collect();
}

/*
 * Clear the dirty bit of the block of `p`, so that the next collection
 * checks the objects of the block (a dirty block is rescanned instead),
 * unless a barrier dirties it again.  Besides the barriers, the stack scan
 * of a collection dirties the block of every object a stack seems to
 * reference, also through a stale word that the collector itself left on
 * its stack.
 */
static void
clean_block(const void *p)
{
  size_t i = PHT_HASH(p);

  GC_dirty_pages[divWORDSZ(i)] &= ~((word)1 << modWORDSZ(i));
}

static int
case_barrier(void)
{
  int i;

  setup_old();
  clean_block(g_old);
  store_young(1, 0);
  clear_stack();
  for (i = 0; i < 4; i++)
    collect();
  report("barrier");
  if (0 == errors())
    CHECK(old_intact());
  return errors() == 0;
}

/*
 * The cases that store without a barrier fail if they return: the
 * collection that reports the store makes the process exit with status
 * 70.  A stale word on a stack can make the target look held by a stack,
 * which defers it to the next collection, so a case tries up to
 * `N_ROUNDS` old objects.  A round that reported nothing must count
 * a window instead: deferred, a target finds the block of the old object
 * dirty at a later collection.
 */
#  define N_ROUNDS 5

/*
 * Exit with status 1 if a store was reported, since the collection that
 * reports one does not return, or unless a window was counted since the
 * count was `w`.  `_Exit()` skips the at-exit handler of the checker,
 * which exits with status 70 if a report stands.
 */
static void
expect_window(GC_word w)
{
  if (errors() != 0) {
    fprintf(stderr, "A store was reported without exiting\n");
    _Exit(1);
  }
  if (windows() == w) {
    fprintf(stderr, "A store was neither reported nor deferred\n");
    _Exit(1);
  }
}

/* Dirty the old object and collect: its deferred target becomes a window. */
static GC_ATTR_NOINLINE void
drop_old(void)
{
  GC_word w = windows();

  GC_END_STUBBORN_CHANGE(g_old);
  collect();
  expect_window(w);
}

static int
case_lost(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    setup_old();
    clean_block(g_old);
    store_young(0, 0);
    clear_stack();
    collect_minor();
    drop_old();
  }
  report("lost");
  return 0;
}

static int
case_missed(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    setup_old();
    clean_block(g_old);
    store_young(0, 1);
    clear_stack();
    collect_minor();
    drop_old();
  }
  report("missed");
  return 0;
}

static GC_ATTR_NOINLINE struct young *
store_young_held(void)
{
  struct old *o = (struct old *)g_old;
  struct young *y = new_young();

  o->p = y;
  return y;
}

static GC_ATTR_NOINLINE void
collect_holding(struct young *volatile *py)
{
  /* Held by a local, thus deferred to the next collection. */
  collect_minor();
  CHECK((*py)->magic == MAGIC);
}

static int
case_missed_next(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    struct young *volatile y;
    GC_word w;

    setup_old();
    clean_block(g_old);
    y = store_young_held();
    clear_stack();
    collect_holding(&y);
    y = NULL;
    /* The stack scan of that collection may have dirtied it. */
    clean_block(g_old);
    clear_stack();
    w = windows();
    collect();
    /* Unless reported, the deferred target found the block dirty. */
    expect_window(w);
  }
  report("missed-next");
  return 0;
}

/*
 * A store whose barrier runs after a collection found the target on
 * a stack, as when a thread is stopped between the two: the target is
 * deferred, and the next collection, minor or full, finds the block of
 * the object dirty, which counts as a window, not as a missed store.
 */
static int
case_window(void)
{
  int full;
  int ok = 1;

  for (full = 0; full < 2; full++) {
    struct young *volatile y;
    GC_word w = windows();

    setup_old();
    clean_block(g_old);
    y = store_young_held();
    clear_stack();
    collect_holding(&y);
    /* Only the block of the object may make the target a window. */
    clean_block((const char *)g_old + HBLKSIZE);
    GC_END_STUBBORN_CHANGE(&((struct old *)g_old)->p);
    y = NULL;
    clear_stack();
    if (full) {
      GC_gcollect();
    } else {
      collect_minor();
    }
    if (windows() == w)
      ok = 0;
  }
  report("window");
  return ok && errors() == 0 && old_intact();
}

/* Call `exit()` while the checker prints a report. */
static const char *GC_CALLBACK
describe_exit(const void *obj, size_t size)
{
  (void)obj;
  (void)size;
  fprintf(stderr, "Calling exit() while a report is printed\n");
  exit(0);
}

static int
case_lost_exit(void)
{
  GC_set_wbar_verify_describe(describe_exit);
  return case_lost();
}

/*
 * The objects of a thread-local free list kept over a collection are
 * marked; the collector dirties their blocks (thus they are rescanned at
 * the next minor collection after a store without a barrier).
 */
struct tl {
  struct young *p;
  void *pad[11];
};

static void *volatile g_obj2;

static GC_ATTR_NOINLINE void
tlfs_prepare(void)
{
  void *p;

  /* Fill the thread-local free list of the size; keep one object. */
  g_obj = GC_MALLOC(sizeof(struct tl));
  CHECK(g_obj != NULL);
  /* Refill it, with two blocks if a refill may take several. */
  do {
    p = GC_MALLOC(sizeof(struct tl));
    CHECK(p != NULL);
  } while (HBLKPTR(p) == HBLKPTR(g_obj));
  /*
   * Fill a thread-local free list that the root push walks after that one,
   * so that the stale pointers of the walk point into another block.
   */
  CHECK(GC_MALLOC_ATOMIC(2 * sizeof(struct tl)) != NULL);
}

static GC_ATTR_NOINLINE void
tlfs_store(void)
{
  struct tl *t = (struct tl *)GC_MALLOC(sizeof(struct tl));
  struct tl *t2;

  CHECK(t != NULL);
  t->p = new_young(); /*< no barrier: a new object */
  g_obj = t;
  /* The first object of the next block of the list. */
  do {
    t2 = (struct tl *)GC_MALLOC(sizeof(struct tl));
    CHECK(t2 != NULL);
  } while (HBLKPTR(t2) == HBLKPTR(t));
  t2->p = new_young(); /*< no barrier: a new object */
  /* Checked after the first, its target leaves the stale words instead. */
  t2->pad[10] = new_young();
  g_obj2 = t2;
}

static GC_ATTR_NOINLINE int
tlfs_intact(void)
{
  const struct tl *t = (const struct tl *)g_obj;
  const struct tl *t2 = (const struct tl *)g_obj2;

  return t->p != NULL && t->p->magic == MAGIC
         && (NULL == t2 || (t2->p != NULL && t2->p->magic == MAGIC));
}

static int
case_tlfs(void)
{
  int i;

  tlfs_prepare();
  clear_stack();
  collect();
  tlfs_store();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("tlfs");
  if (0 == errors())
    CHECK(tlfs_intact());
  return errors() == 0;
}

/* An object freed explicitly while marked, then reused. */
static GC_ATTR_NOINLINE void
free_prepare(void)
{
  g_obj = GC_generic_malloc(200, NORMAL);
  CHECK(g_obj != NULL);
}

static GC_ATTR_NOINLINE int
free_reuse(void)
{
  void *a = g_obj;
  struct tl *b;

  g_obj = NULL;
  /* Only the free may dirty its block for the next collection. */
  clean_block(a);
  GC_free(a);
  b = (struct tl *)GC_generic_malloc(200, NORMAL);
  CHECK(b != NULL);
  b->p = new_young(); /*< no barrier: a new object */
  g_obj = b;
  return b == a;
}

static int
case_free(void)
{
  int i;
  int reused;

  free_prepare();
  clear_stack();
  collect();
  collect();
  reused = free_reuse();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("free");
  if (0 == errors())
    CHECK(tlfs_intact());
  if (!reused)
    printf("free: the object was not reused, case not exercised\n");
  return reused && errors() == 0;
}

/*
 * A marked object freed onto a free list which is not empty: its link
 * points to an unmarked free object, which is no store to report.
 */
#  define FREE_KEPT_SZ 320

static GC_ATTR_NOINLINE void
free_kept_prepare(void)
{
  g_obj = GC_generic_malloc(FREE_KEPT_SZ, NORMAL);
  CHECK(g_obj != NULL);
}

static GC_ATTR_NOINLINE void
free_kept_free(void)
{
  void *a = g_obj;

  /* Fill the free list of the size, which the collections emptied. */
  CHECK(GC_generic_malloc(FREE_KEPT_SZ, NORMAL) != NULL);
  g_obj = NULL;
  GC_free(a);
  CHECK(obj_link(a) != NULL);
  clean_block(a);
}

static int
case_free_kept(void)
{
  free_kept_prepare();
  clear_stack();
  collect();
  collect();
  free_kept_free();
  clear_stack();
  collect_minor();
  report("free-kept");
  return errors() == 0;
}

/* An object initialized across a collection which marks it. */
static GC_ATTR_NOINLINE void
stack_init(void)
{
  struct old *o = new_old();

  collect();          /*< marks `o`, referenced by the stack only */
  o->p = new_young(); /*< no barrier: a new object */
  g_old = o;
}

static int
case_stack(void)
{
  int i;

  stack_init();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect();
  report("stack");
  if (0 == errors())
    CHECK(old_intact());
  return errors() == 0;
}

/*
 * An uncollectable object written without a barrier.  It needs no root,
 * thus its address is kept hidden: a stale copy on the stack would dirty
 * its block.
 */
static volatile GC_hidden_pointer g_uncoll;

static GC_ATTR_NOINLINE void
uncoll_prepare(void)
{
  const void *u = GC_MALLOC_UNCOLLECTABLE(sizeof(struct tl));

  CHECK(u != NULL);
  g_uncoll = GC_HIDE_POINTER(u);
}

static GC_ATTR_NOINLINE void
uncoll_store(void)
{
  struct tl *t = (struct tl *)GC_REVEAL_POINTER(g_uncoll);

  /* Only its kind may make the next collection rescan it. */
  clean_block(t);
  t->p = new_young(); /*< no barrier */
}

static GC_ATTR_NOINLINE int
uncoll_intact(void)
{
  const struct tl *t = (const struct tl *)GC_REVEAL_POINTER(g_uncoll);

  return t->p != NULL && t->p->magic == MAGIC;
}

static int
case_uncoll(void)
{
  int i;

  uncoll_prepare();
  clear_stack();
  collect();
  collect();
  uncoll_store();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("uncoll");
  if (0 == errors())
    CHECK(uncoll_intact());
  return errors() == 0;
}

/*
 * An old object of a kind of its own, written without a barrier once its
 * block is clean: no error if the kind is set always rescanned (`kind`),
 * else it must be reported as lost (`kind-unflagged`).  The kind is
 * scanned in full and has no thread-local free lists.
 */
static int g_kind = -1;

static GC_ATTR_NOINLINE void
make_old_of_kind(void)
{
  g_old = GC_generic_malloc(sizeof(struct old), g_kind);
  CHECK(g_old != NULL);
}

static void
setup_old_of_kind(int always)
{
  if (g_kind < 0) {
    g_kind = (int)GC_new_kind(GC_new_free_list(), 0 | GC_DS_LENGTH, 1, 1);
    /* The setting is turned on first, so that `kind-unflagged` clears it. */
    GC_set_kind_always_rescanned(g_kind, 1);
    GC_set_kind_always_rescanned(g_kind, always);
  }
  make_old_of_kind();
  clear_stack();
  collect();
  collect();
}

static int
case_kind(void)
{
  int i;

  setup_old_of_kind(1);
  clean_block(g_old);
  store_young(0, 0);
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("kind");
  if (0 == errors())
    CHECK(old_intact());
  return errors() == 0;
}

static int
case_kind_unflagged(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    setup_old_of_kind(0);
    clean_block(g_old);
    store_young(0, 0);
    clear_stack();
    collect_minor();
    drop_old();
  }
  report("kind-unflagged");
  return 0;
}

/*
 * Threads storing with the inline barrier (the form a compiler emits)
 * while another thread allocates.
 */
#  ifdef GC_PTHREADS
#    define N_STORERS 4
#    define N_OLD 64
#    define N_MINOR 40

static struct old *volatile g_olds[N_STORERS][N_OLD];
static volatile int g_stop;

GC_INLINE void
inline_barrier(const void *slot)
{
  size_t i = PHT_HASH(slot);
  volatile word *w = (volatile word *)&GC_dirty_pages[divWORDSZ(i)];
  word m = (word)1 << modWORDSZ(i);

  if ((*w & m) == 0)
    (void)__atomic_fetch_or(w, m, __ATOMIC_RELAXED);
}

/* A young object from a thread-local free list. */
static GC_ATTR_NOINLINE struct young *
new_young_tl(void)
{
  struct young *y = (struct young *)GC_MALLOC(sizeof(struct young));

  CHECK(y != NULL);
  y->magic = MAGIC;
  return y;
}

static GC_word
checks(void)
{
  struct GC_wbar_totals_s t;

  GC_get_wbar_verify_totals(&t);
  return t.checks;
}

/* The collections completed when the storers start. */
static GC_word g_gc0;

static void *
storer(void *arg)
{
  int id = (int)(GC_uintptr_t)arg;
  unsigned long n = 0;

  /*
   * Stop also after the collections the allocator allows: without
   * thread-local allocation every allocation takes the allocator lock, and
   * the allocator thread may wait for it for minutes while the storers
   * allocate and collect.
   */
  while (!g_stop && gc_no() - g_gc0 < 10 * N_MINOR) {
    struct old *o = g_olds[id][n % N_OLD];
    struct young *y = new_young_tl();
    size_t k = (size_t)(n / N_OLD) % 4;

    if (0 == k) {
      o->p = y;
      inline_barrier(&o->p);
    } else {
      o->pad[k] = y;
      GC_END_STUBBORN_CHANGE(&o->pad[k]);
    }
    GC_reachable_here(y);
    CHECK(o->p == NULL || o->p->magic == MAGIC);
    n++;
  }
  return arg;
}

static void *
allocator(void *arg)
{
  GC_word start = checks();
  GC_word gc0 = gc_no();

  (void)arg;
  while (checks() - start < N_MINOR) {
    CHECK(gc_no() - gc0 < 10 * N_MINOR);
    CHECK(GC_MALLOC(64) != NULL);
  }
  g_stop = 1;
  return NULL;
}

static int
case_threads(void)
{
  pthread_t t[N_STORERS + 1];
  int i, j;

  for (i = 0; i < N_STORERS; i++) {
    for (j = 0; j < N_OLD; j++)
      g_olds[i][j] = new_old();
  }
  collect();
  collect();
  g_gc0 = gc_no();
  for (i = 0; i < N_STORERS; i++)
    CHECK(0 == pthread_create(&t[i], NULL, storer, (void *)(GC_uintptr_t)i));
  CHECK(0 == pthread_create(&t[N_STORERS], NULL, allocator, NULL));
  for (i = 0; i <= N_STORERS; i++)
    CHECK(0 == pthread_join(t[i], NULL));
  collect();
  collect();
  for (i = 0; i < N_STORERS; i++) {
    for (j = 0; j < N_OLD; j++) {
      const struct old *o = g_olds[i][j];
      size_t k;

      CHECK(o->p == NULL || o->p->magic == MAGIC);
      for (k = 1; k < 4; k++) {
        const struct young *y = (const struct young *)o->pad[k];

        CHECK(NULL == y || y->magic == MAGIC);
      }
    }
  }
  report("threads");
  return errors() == 0;
}
#  else
static int
case_threads(void)
{
  printf("threads: no threads, skipped\n");
  return 1;
}
#  endif

struct test_case {
  const char *name;
  int (*fn)(void);
  int clean;
};

static const struct test_case cases[] = {
  { "tlfs", case_tlfs, 1 },
  { "barrier", case_barrier, 1 },
  { "free", case_free, 1 },
  { "free-kept", case_free_kept, 1 },
  { "stack", case_stack, 1 },
  { "window", case_window, 1 },
  { "uncoll", case_uncoll, 1 },
  { "threads", case_threads, 1 },
  { "kind", case_kind, 1 },
  { "kind-unflagged", case_kind_unflagged, 0 },
  { "lost", case_lost, 0 },
  { "missed", case_missed, 0 },
  { "missed-next", case_missed_next, 0 },
  { "lost-exit", case_lost_exit, 0 },
};

#  define N_CASES (sizeof(cases) / sizeof(cases[0]))

int
main(int argc, char **argv)
{
  size_t i;
  int ok = 1;

  if (getenv("GC_WBAR_VERIFY") == NULL) {
#  ifdef _WIN32
    (void)_putenv("GC_WBAR_VERIFY=1");
#  else
    (void)setenv("GC_WBAR_VERIFY", "1", 1);
#  endif
  }
  GC_set_manual_vdb_allowed(1);
  /*
   * The thread-local free lists of the `tlfs` case are refilled from the
   * first allocation of their size.
   */
  GC_set_no_thread_local_warmup(1);
  GC_INIT();
  GC_enable_incremental();
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  GC_set_full_freq(100000);
  /* The checker turns itself off under a time limit. */
  GC_set_time_limit(GC_TIME_UNLIMITED);
  for (i = 0; i < N_CASES; i++) {
    if ((argc < 2 && cases[i].clean)
        || (argc >= 2 && 0 == strcmp(argv[1], cases[i].name))) {
      int r;

      GC_get_wbar_verify_totals(&base);
      r = cases[i].fn();

      printf("%s: %s\n", cases[i].name, r ? "ok" : "FAILED");
      if (!r)
        ok = 0;
      if (argc >= 2)
        break;
    }
  }
  if (argc >= 2 && i == N_CASES) {
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
  }
  return ok ? 0 : 1;
}

#else

int
main(void)
{
  printf("The manual VDB mode is not built or NO_INCREMENTAL is defined,"
         " test skipped\n");
  return 0;
}

#endif
