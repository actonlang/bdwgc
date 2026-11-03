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
 * of a kind set always rescanned (`kind`).  An object held by a local and
 * filled after the stop function abandoned a first world-stopped mark
 * (`abandon`).  An object allocated during a collection marked in
 * world-stopped steps (a time limit of 10 microseconds) and filled after
 * a step scanned it (`alloc`, skipped in builds without the parallel
 * marker, and where the clock does not resolve 10 microseconds, e.g. on
 * Windows).  Large objects, of which only the dirty blocks are rescanned,
 * unless they are of a kind always rescanned: stores with a barrier into
 * a far block (`large-barrier`, `large-stubborn`), a new large object filled
 * in a far block after a collection marked it from the stack
 * (`large-stack`), a large uncollectable object filled in a far block
 * without a barrier (`large-uncoll`).  A report whose object was freed
 * before the full collection that checks it is stale (`stale`).  A dead
 * object marked again by a conservative reference, whose old pointers now
 * point to new objects, holds no missed store (`revived`).  With an
 * argument, one case runs; the cases `lost`, `missed`, `missed-next`,
 * `kind-unflagged`, `large-missed`, `large-missed-next`, `missed-abandoned`
 * and `final-lost` store without a barrier and must be reported (as lost, as
 * missed, as missed at the next collection, as lost, as lost in a clean
 * block of a large object whose first block is dirty, as missed at the next
 * collection in such a block, as missed at the read of a first world-stopped
 * mark that the stop function abandons, and as lost at the final read of
 * a collection marked in steps), after which the full collection that
 * confirms the report makes the process exit with status 70.  So must the
 * case `lost-exit`, in which `exit()` is called while the report is printed,
 * and the case `lost-at-exit`, whose report the full collection at exit
 * confirms.  Skipped unless the manual VDB mode is built, and if
 * `NO_INCREMENTAL` is defined.
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

/* The table of `GC_get_dirty_bits()` and its sizes. */
static volatile GC_word *g_dirty_bits;
static unsigned g_log_block_size, g_log_entries;

/* The word of the dirty bits for the block of `p`, and the bit in it. */
static volatile GC_word *
dirty_word(const void *p, GC_word *pm)
{
  GC_word i = ((GC_word)(GC_uintptr_t)p >> g_log_block_size)
              & (((GC_word)1 << g_log_entries) - 1);

  *pm = (GC_word)1 << (i % (8 * sizeof(GC_word)));
  return g_dirty_bits + i / (8 * sizeof(GC_word));
}

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

/*
 * Overwrite the stack below the caller, to drop stale pointers.  Not
 * instrumented by AddressSanitizer, whose redzones in the frame would keep
 * the stale words that the frame of the caller's next callee covers.
 */
GC_ATTR_NO_SANITIZE_ADDR
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
     * instead.  The first target holds a child of its own, which the
     * marking must reach from it once it is kept alive (see
     * `expect_kept()`).
     */
    y->pad[0] = new_young();
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
  t->stale -= base.stale;
}

/*
 * Let the full collection that a report makes due confirm or discard the
 * reports waiting for it.
 */
static void
confirm(void)
{
  int i;

  for (i = 0;; i++) {
    struct GC_wbar_totals_s t;

    GC_get_wbar_verify_totals(&t);
    if (0 == t.pending)
      return;
    CHECK(i < 4);
    collect();
  }
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

static GC_word
stales(void)
{
  struct GC_wbar_totals_s t;

  totals(&t);
  return t.stale;
}

static GC_word
pending(void)
{
  struct GC_wbar_totals_s t;

  GC_get_wbar_verify_totals(&t);
  return t.pending;
}

static void
report(const char *name)
{
  struct GC_wbar_totals_s t;

  confirm();
  totals(&t);
  printf("%s: checks=%lu lost=%lu missed=%lu window=%lu stale=%lu\n", name,
         (unsigned long)t.checks, (unsigned long)t.lost,
         (unsigned long)t.missed, (unsigned long)t.window,
         (unsigned long)t.stale);
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
  GC_word m;
  volatile GC_word *w = dirty_word(p, &m);

  *w &= ~m;
}

/* Set the dirty bit of the block of `p`, as the inline barrier does. */
static void
dirty_block(const void *p)
{
  GC_word m;
  volatile GC_word *w = dirty_word(p, &m);

  *w |= m;
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
 * The cases that store without a barrier fail if they return: the full
 * collection that confirms the report of the store makes the process exit
 * with status 70.  A stale word on a stack can make the target look held
 * by a stack, which defers it to the next collection, so a case tries up
 * to `N_ROUNDS` old objects.  A round that reported nothing must count
 * a window instead: deferred, a target finds the block of the old object
 * dirty at a later collection.
 */
#  define N_ROUNDS 5

/*
 * Let a full collection confirm the reports waiting, then exit with
 * status 1 if a store was reported, since the collection that confirms
 * one does not return, or unless a window was counted since the count was
 * `w`.  `_Exit()` skips the at-exit handler of the checker, which exits
 * with status 70 if a report stands.
 */
static void
expect_window(GC_word w)
{
  confirm();
  if (errors() != 0) {
    fprintf(stderr, "A store was reported without exiting\n");
    _Exit(1);
  }
  if (windows() == w) {
    fprintf(stderr, "A store was neither reported nor deferred\n");
    _Exit(1);
  }
}

/*
 * Exit with status 1 unless the target `p`, after the collection that
 * checked the store, is intact and marked, and so is the child it holds
 * if any: the target of a report must stay alive, with the objects it
 * references, until a full collection confirms the report.  As in
 * `expect_window()`, the at-exit handler of the checker is skipped.
 */
static void
expect_kept(const struct young *p)
{
  const struct young *c = NULL == p ? NULL : (const struct young *)p->pad[0];

  if (NULL == p || p->magic != MAGIC || !GC_is_marked(p)
      || (c != NULL && (c->magic != MAGIC || !GC_is_marked(c)))) {
    fprintf(stderr, "The target of a report was not kept alive\n");
    _Exit(1);
  }
}

/* Dirty the old object and collect: its deferred target becomes a window. */
static GC_ATTR_NOINLINE void
drop_old(void)
{
  GC_word w = windows();

  expect_kept(((const struct old *)g_old)->p);
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
 * A report still waiting when the process exits (`lost-at-exit`): the case
 * returns once a report waits, and the at-exit handler of the checker lets
 * a full collection confirm it.
 */
static int
case_lost_at_exit(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    setup_old();
    clean_block(g_old);
    store_young(0, 0);
    clear_stack();
    collect_minor();
    if (pending() != 0)
      return 0;
    drop_old();
  }
  report("lost-at-exit");
  return 0;
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
 * Collections marked in world-stopped steps.  `start_sliced()` sets the
 * time limit to 10 microseconds and allocates until a minor collection
 * starts (a world-stopped mark is notified); it returns whether the
 * collection is in progress after its first mark.  `finish_sliced()`
 * restores the unlimited time and completes the collection.  A long chain
 * of new objects, reachable from a root, makes the marking span many
 * steps.  The steps are built only with the parallel marker; without it
 * the cases are skipped.
 */
#  ifdef PARALLEL_MARK_DEADLINE
#    define HAVE_STEPS 1
#  else
#    define HAVE_STEPS 0
#  endif

#  define NO_STEPS_MSG \
    "no marking in steps without the parallel marker, skipped"

struct chain {
  struct chain *next;
  void *x;
};

static struct chain *volatile g_chain;
static volatile GC_hidden_pointer g_mid;
static volatile GC_word g_mark_starts;

static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_MARK_START == e)
    g_mark_starts++;
}

static GC_ATTR_NOINLINE void
build_chain(size_t n)
{
  struct chain *head = NULL;
  size_t i;

  GC_disable();
  for (i = 0; i < n; i++) {
    struct chain *c = (struct chain *)GC_MALLOC(sizeof(struct chain));

    CHECK(c != NULL);
    c->next = head;
    if (n / 2 == i)
      g_mid = GC_HIDE_POINTER(c);
    head = c;
  }
  g_chain = head;
  GC_enable();
}

static void
set_limit_us(unsigned long us)
{
  struct GC_timeval_s tv;

  tv.tv_ms = us / 1000;
  tv.tv_nsec = (us % 1000) * 1000;
  GC_set_time_limit_tv(tv);
}

#  ifdef PARALLEL_MARK_DEADLINE
/*
 * Whether the clock of the collector resolves the time limit of the steps:
 * one of a few tries must see it advance by less (a try fails if the
 * thread is preempted meanwhile).  On Windows it counts milliseconds, and
 * a step runs until the clock advances, often to the end of the marking.
 */
static int
fine_clock(void)
{
  int i;

  for (i = 0; i < 10; i++) {
    CLOCK_TYPE start;
    unsigned long us;

    GET_TIME(start);
    do {
      CLOCK_TYPE now;

      GET_TIME(now);
      us = MS_TIME_DIFF(now, start) * 1000UL
           + NS_FRAC_TIME_DIFF(now, start) / 1000UL;
    } while (0 == us);
    if (us < 10)
      return 1;
  }
  return 0;
}
#  endif

static GC_ATTR_NOINLINE int
start_sliced(void)
{
  GC_word starts = g_mark_starts;
  GC_word n = gc_no();
  int i;

  GC_alloc_lock();
  GC_need_full_gc = FALSE;
  GC_alloc_unlock();
  set_limit_us(10);
  for (i = 0; g_mark_starts == starts; i++) {
    CHECK(i < 100000000);
    CHECK(GC_MALLOC_ATOMIC(16) != NULL);
  }
  return gc_no() == n;
}

static GC_ATTR_NOINLINE void
finish_sliced(void)
{
  int i;

  GC_set_time_limit(GC_TIME_UNLIMITED);
  for (i = 0; GC_collect_a_little(); i++)
    CHECK(i < 10000000);
}

/*
 * Build a chain and start a collection that is marked in steps, with the
 * block of `p` clean at the first read of the dirty bits: else the pass
 * over the dirty blocks would push the block, and a step would rescan it.
 */
static void
start_chain_sliced(const void *p)
{
  int i;

  for (i = 0;; i++) {
    CHECK(i < 20);
    build_chain(200000);
    clear_stack();
    clean_block(p);
    if (start_sliced())
      return;
    finish_sliced();
  }
}

/*
 * An object allocated during a collection marked in steps (`alloc`),
 * linked to the middle of a chain the marking follows, held by a local,
 * and filled without a barrier after a step scanned it and before the next
 * world-stopped mark.  Its block is dirty for the final read since it was
 * handed out during the collection.  The second store, into the last word,
 * takes the stale words that the checker leaves on its stack, as in
 * `store_young()`.
 */
static GC_ATTR_NOINLINE int
alloc_round(void)
{
  struct chain *t = (struct chain *)GC_REVEAL_POINTER(g_mid);
  GC_word starts = g_mark_starts;
  struct old *x = new_old();
  int scanned = 0;
  int i;

  t->x = x;
  GC_END_STUBBORN_CHANGE(&t->x);
  /* Steps only: a world-stopped mark would dirty the block of `x`. */
  for (i = 0; GC_collect_a_little() && g_mark_starts == starts; i++) {
    CHECK(i < 10000000);
    if (GC_is_marked(x)) {
      /* Let a step scan it. */
      scanned = GC_collect_a_little() && g_mark_starts == starts;
      break;
    }
  }
  x->p = new_young();    /*< no barrier: a new object */
  x->last = new_young(); /*< likewise */
  GC_reachable_here(x);
  return scanned;
}

static GC_ATTR_NOINLINE int
alloc_intact(void)
{
  const struct chain *t = (const struct chain *)GC_REVEAL_POINTER(g_mid);
  const struct old *x = (const struct old *)t->x;

  return x->p != NULL && x->p->magic == MAGIC && x->last != NULL
         && x->last->magic == MAGIC;
}

static int
case_alloc(void)
{
  int round;
  int exercised = 0;

  if (!HAVE_STEPS) {
    printf("alloc: %s\n", NO_STEPS_MSG);
    return 1;
  }
#  ifdef PARALLEL_MARK_DEADLINE
  if (!fine_clock()) {
    printf("alloc: the clock does not resolve 10 microseconds, skipped\n");
    return 1;
  }
#  endif
  for (round = 0; round < 20 && !exercised; round++) {
    build_chain(200000);
    clear_stack();
    if (start_sliced())
      exercised = alloc_round();
    clear_stack();
    finish_sliced();
    /* A deferred target is resolved while the chain holds the object. */
    collect();
    if (!exercised)
      g_chain = NULL;
  }
  report("alloc");
  if (!exercised)
    printf("alloc: no step scanned the object first, case not exercised\n");
  if (0 == errors() && exercised)
    CHECK(alloc_intact());
  g_chain = NULL;
  return exercised && errors() == 0;
}

/*
 * A world-stopped mark which the stop function abandons at once
 * (`abandon`) pushes the roots first: the stack scan dirties, for the next
 * read of the dirty bits, the block of an object a local holds, and the
 * final mark rescans a store into it without a barrier after the world
 * restarts.  The block is dirty when the world restarts, while the read
 * at the start of the mark cleared it.  (Without the parallel marker the
 * mark is not abandoned.)
 */
static int GC_CALLBACK
always_stop(void)
{
  return 1;
}

static GC_ATTR_NOINLINE int
abandon_held(void)
{
  struct old *volatile x = new_old();
  GC_stop_func stop_func = GC_get_stop_func();
  GC_word m;
  const volatile GC_word *w = dirty_word(x, &m);
  int dirty;

  GC_set_stop_func(always_stop);
  GC_start_incremental_collection();
  GC_set_stop_func(stop_func);
  dirty = (*w & m) != 0;
  x->p = new_young(); /*< no barrier: a new object */
  g_old = x;
  return dirty;
}

static int
case_abandon(void)
{
  int dirty;
  int i;

  dirty = abandon_held();
  clear_stack();
  while (GC_collect_a_little()) {
    /* Empty. */
  }
  for (i = 0; i < 2; i++)
    collect_minor();
  report("abandon");
  if (!dirty)
    printf("abandon: no root push before the mark was abandoned\n");
  if (0 == errors())
    CHECK(old_intact());
  return dirty && errors() == 0;
}

/*
 * Run a collection whose first world-stopped mark the stop function
 * abandons once the roots are pushed, and let it complete in steps:
 * a minor one, unless a report waits for a full one.
 */
static GC_ATTR_NOINLINE void
collect_abandoned_first(void)
{
  GC_stop_func stop_func = GC_get_stop_func();

  GC_alloc_lock();
  GC_need_full_gc = FALSE;
  GC_alloc_unlock();
  GC_set_stop_func(always_stop);
  GC_start_incremental_collection();
  GC_set_stop_func(stop_func);
  clear_stack();
  while (GC_collect_a_little()) {
    /* Empty. */
  }
}

/*
 * A store without a barrier found at the read of a first world-stopped
 * mark that the stop function abandons (`missed-abandoned`): its target,
 * which a static root keeps, is marked by the steps that follow, and the
 * store must still be reported, as missed.  The full collection that
 * confirms the report starts with an abandoned mark too, and must confirm
 * it when it completes in steps.  (Without the parallel marker the marks
 * are not abandoned.)
 */
static int
case_missed_abandoned(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    GC_word w;

    setup_old();
    clean_block(g_old);
    store_young(0, 1);
    clear_stack();
    w = windows();
    collect_abandoned_first();
    if (0 == pending()) {
      /*
       * Deferred: unless the final read of the collection counted it as
       * a window, the next one resolves it.
       */
      if (windows() == w)
        drop_old();
      continue;
    }
    expect_kept(((const struct old *)g_old)->p);
    collect_abandoned_first();
    fprintf(stderr, "A full collection marked in steps confirmed no report\n");
    _Exit(1);
  }
  report("missed-abandoned");
  return 0;
}

/*
 * A store without a barrier into an old object during a collection marked
 * in steps, which the final read reports (`final-lost`).
 */
static int
case_final_lost(void)
{
  int round;

  if (!HAVE_STEPS) {
    printf("final-lost: %s\n", NO_STEPS_MSG);
    return 1;
  }
  for (round = 0; round < N_ROUNDS; round++) {
    setup_old();
    start_chain_sliced(g_old);
    /* The root push of the first mark may have dirtied it. */
    clean_block(g_old);
    store_young(0, 0);
    clear_stack();
    finish_sliced();
    g_chain = NULL;
    drop_old();
  }
  report("final-lost");
  return 0;
}

/*
 * Large objects: a pass over the dirty blocks pushes only their dirty
 * blocks.  `large-barrier` stores into a far block of an old one and sets
 * the bit of that block, as the inline barrier does; `large-stubborn`
 * stores into a far block and calls `GC_end_stubborn_change()` for the
 * start of the object, which dirties all of its blocks; `large-missed`
 * dirties the first block of the object and stores into a far block
 * without a barrier, which must be reported as lost; `large-stack` fills
 * a far block of a new one, held by a local, after a collection marked it
 * from the stack, which dirtied all of its blocks.
 */
#  define LARGE_WORDS (64 * 16384 / sizeof(void *))
#  define LARGE_FAR (LARGE_WORDS - 8)

static void **volatile g_large;

/*
 * Without the debug header that `GC_MALLOC()` adds with `GC_DEBUG`, which
 * would move the last words of the object out of the block of
 * `a[LARGE_FAR]`.
 */
static void **
new_large(void)
{
  void **a = (void **)GC_generic_malloc(LARGE_WORDS * sizeof(void *), NORMAL);

  CHECK(a != NULL);
  return a;
}

static GC_ATTR_NOINLINE void
large_make_old(void)
{
  g_large = new_large();
}

/* `barrier`: 0 none, 1 the bit of the block, 2 `GC_end_stubborn_change()`. */
static GC_ATTR_NOINLINE void
large_store(int barrier)
{
  void **a = g_large;
  struct young *y = new_young();

  /* Only the barrier may make the next collection rescan the far block. */
  clean_block(&a[LARGE_FAR]);
  a[0] = NULL;
  dirty_block(&a[0]);
  a[LARGE_FAR] = y;
  /* Checked after the first, its target takes the stale words. */
  a[LARGE_WORDS - 4] = new_young();
  if (1 == barrier)
    dirty_block(&a[LARGE_FAR]);
  if (2 == barrier)
    GC_END_STUBBORN_CHANGE(a);
  GC_reachable_here(y);
}

static GC_ATTR_NOINLINE int
large_intact(void)
{
  const struct young *y = (const struct young *)g_large[LARGE_FAR];

  return y != NULL && y->magic == MAGIC;
}

static void
setup_large(void)
{
  large_make_old();
  clear_stack();
  collect();
  collect();
}

static int
large_case(int barrier)
{
  int i;

  setup_large();
  large_store(barrier);
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report(1 == barrier ? "large-barrier" : "large-stubborn");
  if (0 == errors())
    CHECK(large_intact());
  return 0 == errors();
}

static int
case_large_barrier(void)
{
  return large_case(1);
}

static int
case_large_stubborn(void)
{
  return large_case(2);
}

/* Dirty the whole large object and collect, as `drop_old()` does. */
static GC_ATTR_NOINLINE void
drop_large(void)
{
  GC_word w = windows();

  expect_kept((const struct young *)g_large[LARGE_FAR]);
  GC_END_STUBBORN_CHANGE(g_large);
  collect();
  expect_window(w);
}

static int
case_large_missed(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    setup_large();
    large_store(0);
    clear_stack();
    collect_minor();
    drop_large();
  }
  report("large-missed");
  return 0;
}

/*
 * A store without a barrier into a far block of an old large object, whose
 * target a local holds at the collection that finds it, which defers it
 * (`large-missed-next`): at the next read the far block is clean, while
 * the first block is dirty, and the store must be reported as missed.
 */
static GC_ATTR_NOINLINE struct young *
large_store_held(void)
{
  void **a = g_large;
  struct young *y = new_young();

  clean_block(&a[LARGE_FAR]);
  a[LARGE_FAR] = y;
  return y;
}

static int
case_large_missed_next(void)
{
  int round;

  for (round = 0; round < N_ROUNDS; round++) {
    struct young *volatile y;
    GC_word w;

    setup_large();
    y = large_store_held();
    clear_stack();
    collect_holding(&y);
    y = NULL;
    /* The stack scan of that collection may have dirtied the far block. */
    clean_block(&g_large[LARGE_FAR]);
    dirty_block(g_large);
    clear_stack();
    w = windows();
    collect();
    expect_window(w);
  }
  report("large-missed-next");
  return 0;
}

static GC_ATTR_NOINLINE void
large_stack_init(void)
{
  void **a = new_large();

  collect();                  /*< marks `a`, referenced by the stack only */
  a[LARGE_FAR] = new_young(); /*< no barrier: a new object */
  /* Checked after the first, its target takes the stale words. */
  a[LARGE_WORDS - 4] = new_young();
  g_large = a;
}

static int
case_large_stack(void)
{
  int i;

  large_stack_init();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("large-stack");
  if (0 == errors())
    CHECK(large_intact());
  return errors() == 0;
}

/*
 * A large uncollectable object filled in a far block without a barrier
 * (`large-uncoll`): the objects of the always-rescanned kinds are pushed
 * whole, not by dirty block.  Its address is kept hidden, as in `uncoll`.
 */
static volatile GC_hidden_pointer g_large_uncoll;

static GC_ATTR_NOINLINE void
large_uncoll_prepare(void)
{
  const void *u = GC_MALLOC_UNCOLLECTABLE(LARGE_WORDS * sizeof(void *));

  CHECK(u != NULL);
  g_large_uncoll = GC_HIDE_POINTER(u);
}

static GC_ATTR_NOINLINE void
large_uncoll_store(void)
{
  void **a = (void **)GC_REVEAL_POINTER(g_large_uncoll);

  /* Only its kind may make the next collection rescan the far block. */
  clean_block(&a[LARGE_FAR]);
  a[LARGE_FAR] = new_young(); /*< no barrier */
}

static GC_ATTR_NOINLINE int
large_uncoll_intact(void)
{
  void **a = (void **)GC_REVEAL_POINTER(g_large_uncoll);
  const struct young *y = (const struct young *)a[LARGE_FAR];

  return y != NULL && y->magic == MAGIC;
}

static int
case_large_uncoll(void)
{
  int i;

  large_uncoll_prepare();
  clear_stack();
  collect();
  collect();
  large_uncoll_store();
  clear_stack();
  for (i = 0; i < 3; i++)
    collect_minor();
  report("large-uncoll");
  if (0 == errors())
    CHECK(large_uncoll_intact());
  return errors() == 0;
}

/*
 * Reports whose object is dead at the full collection that checks them are
 * stale (`stale`).  First a store without a barrier into an old object
 * which then becomes unreachable.  The checker leaves the target of the
 * last word it records on its stack, where the stack scan of the same
 * collection finds it, and a later read touches the object of a deferred
 * candidate.  So another old object above it in the same block, which
 * stays reachable, gets a pointer to a freed object (a report of it is
 * stale too), and the block is dirtied for the next read (so a deferred
 * candidate of it is a window).  Then a store without a barrier into a far
 * block of an old large object which is then freed.
 */
static void *volatile g_decoy;

/*
 * Make two old objects in one block, the lower one not the first object of
 * the block: `g_old` and `g_decoy`.
 */
static GC_ATTR_NOINLINE void
make_old_pair(void)
{
  int i;

  for (i = 0;; i++) {
    struct old *a = new_old();
    struct old *b = new_old();
    struct old *lo = ADDR_LT((ptr_t)a, (ptr_t)b) ? a : b;

    CHECK(i < 1000);
    if (HBLKPTR(a) == HBLKPTR(b) && (ptr_t)lo != (ptr_t)HBLKPTR(lo)) {
      g_old = lo;
      g_decoy = lo == a ? b : a;
      return;
    }
  }
}

static GC_ATTR_NOINLINE void
store_dead_pair(void)
{
  struct old *o = (struct old *)g_old;
  struct old *d = (struct old *)g_decoy;
  struct young *f = new_young();

  o->p = new_young(); /*< no barrier */
  /* A pointer to an object freed before any collection marked it. */
  d->p = f; /*< no barrier */
  GC_free(f);
}

static int
case_stale(void)
{
  int round;
  GC_word dead;

  for (round = 0; round < N_ROUNDS && 0 == stales(); round++) {
    make_old_pair();
    clear_stack();
    collect();
    collect();
    clean_block(g_old);
    store_dead_pair();
    clear_stack();
    collect_minor();
    GC_END_STUBBORN_CHANGE(g_decoy);
    g_old = NULL;
    clear_stack();
    collect();
  }
  /*
   * The decoy still points to the freed object, which the allocations
   * below may hand out again: it would then point to a new object nobody
   * stored into it, which is reported, and stands.
   */
  if (g_decoy != NULL)
    ((struct old *)g_decoy)->p = NULL;
  g_decoy = NULL;
  dead = stales();
  for (round = 0; round < N_ROUNDS && stales() == dead; round++) {
    void **a;

    setup_large();
    large_store(0);
    clear_stack();
    collect_minor();
    a = g_large;
    g_large = NULL;
    GC_free(a);
    clear_stack();
    collect();
  }
  report("stale");
  return 0 == errors() && dead >= 1 && stales() > dead;
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
  GC_word m;
  volatile GC_word *w = dirty_word(slot, &m);

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

/*
 * A dead object revived by a conservative reference (`revived`).  An
 * object dies in a block whose other objects live, so that the sweep
 * leaves the block alone (it is nearly full), with pointers to two objects
 * that are freed afterwards, and handed out again.  Then a reference to
 * the dead object appears where the collector looks for roots (here
 * a static word, as a stale word in a stack frame would).  A minor
 * collection finds its old pointers in a clean block, pointing to new
 * objects nobody stored into it.  No barrier is missing, so the checker
 * must not report them: while it runs, every collection sweeps every block
 * with unmarked objects at once, and the dead object holds no pointer.
 * The objects are freed explicitly once the dead object holds the last
 * pointers to them, so that they are on a free list (and stay unmarked)
 * while the collection that revives the dead object scans it.  They come
 * from a size class that no other case uses, from one block with an object
 * that stays alive, and are handed out again from that block.  The second
 * pointer takes the stale words that the checker leaves on its stack, as
 * in `store_young()`.  A round in which a stale word kept the dead object
 * alive is tried again.
 */
#  define REVIVED_FILL 768

/* A size class that no other case uses (96 bytes with the extra byte). */
struct filler {
  void *p;
  void *pad[10];
};

/* Likewise (144 bytes); the objects freed and handed out again. */
struct medium {
  struct medium *next;
  void *pad[16];
};

static struct filler *volatile g_fill[REVIVED_FILL];
static struct medium *volatile g_medium;
static volatile GC_hidden_pointer g_victim;
static volatile GC_hidden_pointer g_freed[2];
static void *volatile g_revived;

static GC_ATTR_NOINLINE struct medium *
new_medium(void)
{
  struct medium *m
      = (struct medium *)GC_generic_malloc(sizeof(struct medium), NORMAL);

  CHECK(m != NULL);
  m->next = g_medium;
  g_medium = m;
  return m;
}

/*
 * Make the victim, which points to two objects of the medium size from
 * one block with a third one, which stays alive, in a block of objects
 * which stay alive.
 */
static GC_ATTR_NOINLINE void
make_victim(void)
{
  struct medium *a, *b;
  size_t i;

  do {
    g_medium = NULL;
    a = new_medium();
    b = new_medium();
  } while (HBLKPTR(a) != HBLKPTR(b) || HBLKPTR(b) != HBLKPTR(new_medium()));
  for (i = 0; i < REVIVED_FILL; i++) {
    struct filler *s
        = (struct filler *)GC_generic_malloc(sizeof(struct filler), NORMAL);

    CHECK(s != NULL);
    g_fill[i] = s;
  }
  g_fill[REVIVED_FILL / 2]->p = a;
  g_fill[REVIVED_FILL / 2]->pad[9] = b;
  g_victim = GC_HIDE_POINTER(g_fill[REVIVED_FILL / 2]);
  g_fill[REVIVED_FILL / 2] = NULL;
  g_freed[0] = GC_HIDE_POINTER(a);
  g_freed[1] = GC_HIDE_POINTER(b);
}

/* Free the two objects, the third one stays on `g_medium`. */
static GC_ATTR_NOINLINE void
free_targets(void)
{
  const struct medium *m = g_medium;

  /* `g_medium` lists the third, the second and the first one. */
  g_medium->next = NULL;
  GC_free(GC_REVEAL_POINTER(g_freed[1]));
  GC_free(GC_REVEAL_POINTER(g_freed[0]));
  GC_reachable_here(m);
}

/*
 * Allocate objects of the medium size until both freed objects are handed
 * out again; return whether they are.  The objects are kept on
 * `g_medium`.
 */
static GC_ATTR_NOINLINE int
reuse_targets(void)
{
  const void *a = GC_REVEAL_POINTER(g_freed[0]);
  const void *b = GC_REVEAL_POINTER(g_freed[1]);
  int seen = 0;
  size_t i;

  for (i = 0; i < 4 * HBLKSIZE / sizeof(struct medium) && seen != 3; i++) {
    const void *m = new_medium();

    if (m == a)
      seen |= 1;
    if (m == b)
      seen |= 2;
  }
  return 3 == seen;
}

static int
case_revived(void)
{
  int round;
  int exercised = 0;

  for (round = 0; round < N_ROUNDS && !exercised; round++) {
    make_victim();
    clear_stack();
    /*
     * The victim dies, unless a stale word keeps it.  A full collection,
     * since a minor one keeps it if a collection marked it while it was
     * held.
     */
    GC_gcollect();
    if (GC_is_marked(GC_REVEAL_POINTER(g_victim)))
      continue;
    free_targets();
    /* A conservative reference to the victim appears. */
    g_revived = GC_REVEAL_POINTER(g_victim);
    clear_stack();
    GC_gcollect();
    CHECK(reuse_targets());
    clean_block(g_revived);
    clear_stack();
    collect_minor();
    collect_minor();
    exercised = 1;
  }
  report("revived");
  if (!exercised)
    printf("revived: a stale word kept the victim, case not exercised\n");
  g_revived = NULL;
  g_medium = NULL;
  memset((void *)g_fill, 0, sizeof(g_fill));
  return exercised && errors() == 0;
}

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
  { "alloc", case_alloc, 1 },
  { "abandon", case_abandon, 1 },
  { "large-barrier", case_large_barrier, 1 },
  { "large-stubborn", case_large_stubborn, 1 },
  { "large-stack", case_large_stack, 1 },
  { "large-uncoll", case_large_uncoll, 1 },
  { "stale", case_stale, 1 },
  { "revived", case_revived, 1 },
  { "kind-unflagged", case_kind_unflagged, 0 },
  { "lost", case_lost, 0 },
  { "missed", case_missed, 0 },
  { "missed-next", case_missed_next, 0 },
  { "lost-exit", case_lost_exit, 0 },
  { "lost-at-exit", case_lost_at_exit, 0 },
  { "large-missed", case_large_missed, 0 },
  { "large-missed-next", case_large_missed_next, 0 },
  { "missed-abandoned", case_missed_abandoned, 0 },
  { "final-lost", case_final_lost, 0 },
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
  g_dirty_bits = GC_get_dirty_bits(&g_log_block_size, &g_log_entries);
  CHECK(g_dirty_bits != NULL);
  CHECK(g_dirty_bits == (volatile GC_word *)GC_dirty_pages);
  CHECK(g_log_block_size == LOG_HBLKSIZE);
  CHECK(g_log_entries == LOG_PHT_ENTRIES);
  GC_set_full_freq(100000);
  GC_set_on_collection_event(on_event);
  /* Without a time limit, every collection is one world-stopped mark. */
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
