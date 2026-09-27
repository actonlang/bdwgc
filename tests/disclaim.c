/*
 * Copyright (c) 2011 by Hewlett-Packard Company.  All rights reserved.
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
 * Test that objects reachable from an object allocated with
 * `GC_malloc_with_finalizer` is not reclaimable before the finalizer
 * is called.
 */

#ifdef HAVE_CONFIG_H
/* For `GC_THREADS` (and `GC_PTHREADS`). */
#  include "config.h"
#endif

#undef GC_NO_THREAD_REDIRECTS
#include "gc/gc_disclaim.h"

#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <string.h>

/*
 * Redefine the standard `rand()` with a trivial (yet sufficient for
 * the test purpose) implementation to avoid crashes inside `rand()`
 * on some hosts (e.g. FreeBSD 13.0) when used concurrently.
 * The standard specifies `rand()` as not a thread-safe API function.
 * On other hosts (e.g. OpenBSD 7.3), use of the standard `rand()`
 * causes "rand() may return deterministic values" warning.
 * Note: concurrent update of seed does not hurt the test.
 */
#undef rand
static GC_RAND_STATE_T seed;
#define rand() GC_RAND_NEXT(&seed)

#define MAX_LOG_MISC_SIZES 20 /*< up to 1 MB */
#define POP_SIZE 1000
#define MUTATE_CNT_BASE (6 * 1000000)

#define TEST_ASSERT(e)                                                    \
  if (!(e)) {                                                             \
    fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
            #e);                                                          \
    exit(1);                                                              \
  }

#define CHECK_OUT_OF_MEMORY(p)            \
  do {                                    \
    if (NULL == (p)) {                    \
      fprintf(stderr, "Out of memory\n"); \
      exit(69);                           \
    }                                     \
  } while (0)

static int
is_equal(void *s, int c, size_t len)
{
  while (len--) {
    if (*(char *)s != c)
      return 0;
    s = (char *)s + 1;
  }
  return 1;
}

#define MEM_FILL_BYTE 0x56

static void GC_CALLBACK
misc_sizes_dct(void *obj, void *cd)
{
  unsigned log_size = *(unsigned char *)obj;
  size_t size;

  TEST_ASSERT(log_size < sizeof(size_t) * 8);
  TEST_ASSERT(cd == NULL);
  size = (size_t)1 << log_size;
  TEST_ASSERT(is_equal((char *)obj + 1, MEM_FILL_BYTE, size - 1));
#ifdef CPPCHECK
  GC_noop1_ptr(cd);
#endif
}

static void
test_misc_sizes(void)
{
  static const struct GC_finalizer_closure fc = { misc_sizes_dct, NULL };
  int i;
  for (i = 1; i <= MAX_LOG_MISC_SIZES; ++i) {
    void *p = GC_finalized_malloc((size_t)1 << i, &fc);

    CHECK_OUT_OF_MEMORY(p);
    TEST_ASSERT(is_equal(p, 0, (size_t)1 << i));
    memset(p, MEM_FILL_BYTE, (size_t)1 << i);
    *(unsigned char *)p = (unsigned char)i;
  }
}

/*
 * The number of objects not yet disclaimed ("parents"), and the length
 * of a list reachable from each of them.  Both should be big enough
 * for the collector to overflow the initial mark stack (of `HBLKSIZE`
 * entries by default, with heap blocks of up to 64 KB) while it pushes
 * the parents at the start of a full collection.
 */
#define OVF_PARENTS_CNT (32 * 1024)
#define OVF_LIST_LEN (64 * 1024)

#define OVF_CHILD_MAGIC 0x5a17

struct ovf_child_s {
  GC_word magic;
  /* The number of completed mark phases at the parent disclaim, plus one. */
  GC_word parent_disclaimed_at;
};

struct ovf_parent_s {
  void *list;
  struct ovf_child_s *child;
};

/* These are updated with the allocator lock held. */
static GC_word ovf_marks_done;
static unsigned ovf_parents_disclaimed;
static unsigned ovf_children_disclaimed;

static void GC_CALLBACK
ovf_on_collection_event(GC_EventType e)
{
  if (GC_EVENT_MARK_END == e)
    ovf_marks_done++;
}

static void GC_CALLBACK
ovf_parent_dct(void *obj, void *cd)
{
  struct ovf_child_s *q = ((struct ovf_parent_s *)obj)->child;

  /* The child is reachable only from `obj`. */
  TEST_ASSERT(q->magic == OVF_CHILD_MAGIC);
  TEST_ASSERT(0 == q->parent_disclaimed_at);
  q->parent_disclaimed_at = ovf_marks_done + 1;
  ovf_parents_disclaimed++;
  UNUSED_ARG(cd);
}

static void GC_CALLBACK
ovf_child_dct(void *obj, void *cd)
{
  struct ovf_child_s *q = (struct ovf_child_s *)obj;

  /*
   * The parent keeps the child marked until the collection that finds
   * the parent unreachable, thus the child may be disclaimed only after
   * a later mark phase.
   */
  TEST_ASSERT(q->magic == OVF_CHILD_MAGIC);
  TEST_ASSERT(q->parent_disclaimed_at != 0
              && q->parent_disclaimed_at - 1 < ovf_marks_done);
  q->magic = 0;
  ovf_children_disclaimed++;
  UNUSED_ARG(cd);
}

/*
 * Test that a mark stack overflow while the collector pushes the objects
 * not yet disclaimed does not let it reclaim objects reachable from them.
 */
static void
test_mark_stack_overflow(void)
{
  static const struct GC_finalizer_closure parent_fc
      = { ovf_parent_dct, NULL };
  static const struct GC_finalizer_closure child_fc = { ovf_child_dct, NULL };
  const void *list = NULL;
  int i;

  if (GC_get_find_leak())
    return;
  /* Note: the callback is kept for the objects disclaimed later. */
  GC_set_on_collection_event(ovf_on_collection_event);

  /* Allocate all the objects before the collection. */
  GC_disable();

  /*
   * Each list node refers to a leaf ahead of the next node, so marking
   * the list leaves an entry on the mark stack per node.
   */
  for (i = 0; i < OVF_LIST_LEN; ++i) {
    void **node = (void **)GC_MALLOC(2 * sizeof(void *));
    const void *leaf = GC_MALLOC(sizeof(void *));

    CHECK_OUT_OF_MEMORY(node);
    CHECK_OUT_OF_MEMORY(leaf);
    GC_ptr_store_and_dirty(&node[0], leaf);
    GC_ptr_store_and_dirty(&node[1], list);
    list = node;
  }

  for (i = 0; i < OVF_PARENTS_CNT; ++i) {
    struct ovf_child_s *q = (struct ovf_child_s *)GC_finalized_malloc(
        sizeof(struct ovf_child_s), &child_fc);
    struct ovf_parent_s *p;

    CHECK_OUT_OF_MEMORY(q);
    q->magic = OVF_CHILD_MAGIC;
    p = (struct ovf_parent_s *)GC_finalized_malloc(sizeof(struct ovf_parent_s),
                                                   &parent_fc);
    CHECK_OUT_OF_MEMORY(p);
    GC_ptr_store_and_dirty(&p->list, list);
    GC_ptr_store_and_dirty(&p->child, q);
  }
  GC_enable();

  /* The parents, then the children, become disclaimed. */
  for (i = 0; i < 4; ++i)
    GC_gcollect();
  TEST_ASSERT(ovf_parents_disclaimed >= OVF_PARENTS_CNT / 2);
  TEST_ASSERT(ovf_children_disclaimed >= OVF_PARENTS_CNT / 2);
}

typedef struct pair_s *pair_t;

struct pair_s {
  char magic[16];
  int checksum;
  pair_t car;
  pair_t cdr;
};

static const char *const pair_magic = "PAIR_MAGIC_BYTES";

static int
is_pair(pair_t p)
{
  return memcmp(p->magic, pair_magic, sizeof(p->magic)) == 0;
}

#define CSUM_SEED 782
#define PTR_HASH(p) (GC_HIDE_NZ_POINTER(p) >> 4)

static void GC_CALLBACK
pair_dct(void *obj, void *cd)
{
  pair_t p = (pair_t)obj;
  int checksum = CSUM_SEED;

  TEST_ASSERT(cd == (void *)PTR_HASH(p));
  /* Check that `obj` and its fields are not trashed. */
#ifdef DEBUG_DISCLAIM_DESTRUCT
  printf("Destruct %p: (car= %p, cdr= %p)\n", (void *)p, (void *)p->car,
         (void *)p->cdr);
#endif
  TEST_ASSERT(GC_base(obj));
  TEST_ASSERT(is_pair(p));
  TEST_ASSERT(!p->car || is_pair(p->car));
  TEST_ASSERT(!p->cdr || is_pair(p->cdr));
  if (p->car)
    checksum += p->car->checksum;
  if (p->cdr)
    checksum += p->cdr->checksum;
  TEST_ASSERT(p->checksum == checksum);

  /* Invalidate it. */
  memset(p->magic, '*', sizeof(p->magic));
  p->checksum = 0;
  p->car = NULL;
  p->cdr = NULL;
#ifdef CPPCHECK
  GC_noop1_ptr(cd);
#endif
}

static pair_t
pair_new(pair_t car, pair_t cdr)
{
  pair_t p;
  struct GC_finalizer_closure *pfc
      = GC_NEW_ATOMIC(struct GC_finalizer_closure);

  CHECK_OUT_OF_MEMORY(pfc);
  pfc->proc = pair_dct;
  p = (pair_t)GC_finalized_malloc(sizeof(struct pair_s), pfc);
  CHECK_OUT_OF_MEMORY(p);
  pfc->cd = (void *)PTR_HASH(p);
  TEST_ASSERT(!is_pair(p));
  TEST_ASSERT(is_equal(p, 0, sizeof(struct pair_s)));
  memcpy(p->magic, pair_magic, sizeof(p->magic));
  p->checksum = CSUM_SEED + (car != NULL ? car->checksum : 0)
                + (cdr != NULL ? cdr->checksum : 0);
  p->car = car;
  GC_ptr_store_and_dirty(&p->cdr, cdr);
  GC_reachable_here(car);
#ifdef DEBUG_DISCLAIM_DESTRUCT
  printf("Construct %p: (car= %p, cdr= %p)\n", (void *)p, (void *)p->car,
         (void *)p->cdr);
#endif
  return p;
}

static void
pair_check_rec(pair_t p)
{
  while (p) {
    int checksum = CSUM_SEED;

    if (p->car)
      checksum += p->car->checksum;
    if (p->cdr)
      checksum += p->cdr->checksum;
    TEST_ASSERT(p->checksum == checksum);
    p = (rand() & 1) != 0 ? p->cdr : p->car;
  }
}

#if defined(GC_PTHREADS) && !defined(TEST_NO_THREADS)
#  ifndef NTHREADS
/* Note: this excludes the main thread, which also runs a test. */
#    define NTHREADS 5
#  endif
#  include <errno.h> /*< for `EAGAIN` */
#  include <pthread.h>
#else
#  undef NTHREADS
#  define NTHREADS 0
#endif

#define MUTATE_CNT (MUTATE_CNT_BASE / (NTHREADS + 1))
#define GROW_LIMIT (MUTATE_CNT / 10)

static void *
test(void *data)
{
  int i;
  pair_t pop[POP_SIZE];
  memset(pop, 0, sizeof(pop));
  for (i = 0; i < MUTATE_CNT; ++i) {
    int t = rand() % POP_SIZE;
    int j;

    switch (rand() % (i > GROW_LIMIT ? 5 : 3)) {
    case 0:
    case 3:
      if (pop[t])
        pop[t] = pop[t]->car;
      break;
    case 1:
    case 4:
      if (pop[t])
        pop[t] = pop[t]->cdr;
      break;
    case 2:
      j = rand() % POP_SIZE;
      pop[t] = pair_new(pop[j], pop[rand() % POP_SIZE]);
      break;
    }
    if (rand() % 8 == 1)
      pair_check_rec(pop[rand() % POP_SIZE]);
  }
  return data;
}

int
main(void)
{
#if NTHREADS > 0
  pthread_t th[NTHREADS];
  int i, n;
#endif

  /* Test the same signal usage for threads suspend and restart on Linux. */
#ifdef GC_PTHREADS
  GC_set_thr_restart_signal(GC_get_suspend_signal());
#endif

  /* Make the test stricter. */
  GC_set_all_interior_pointers(0);

#ifdef TEST_MANUAL_VDB
  GC_set_manual_vdb_allowed(1);
#endif
#ifdef TEST_MPROTECT_VDB_DISALLOWED
  GC_set_mprotect_vdb_allowed(0);
#endif
  GC_INIT();
  GC_init_finalized_malloc();
  /* This should go first, while the mark stack is of the initial size. */
  test_mark_stack_overflow();
#ifndef NO_INCREMENTAL
  GC_enable_incremental();
#endif
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  test_misc_sizes();

#if NTHREADS > 0
  printf("Threaded disclaim test\n");
  for (i = 0; i < NTHREADS; ++i) {
    int err = pthread_create(&th[i], NULL, test, NULL);

    if (err != 0) {
      fprintf(stderr, "Thread #%d creation failed, errno= %d\n", i, err);
      if (i > 1 && EAGAIN == err)
        break;
      exit(69);
    }
  }
  n = i;
#endif
  test(NULL);
#if NTHREADS > 0
  for (i = 0; i < n; ++i) {
    int err = pthread_join(th[i], NULL);

    if (err != 0) {
      fprintf(stderr, "Thread #%d join failed, errno= %d\n", i, err);
      exit(2);
    }
  }
#endif
  printf("SUCCEEDED\n");
  return 0;
}
