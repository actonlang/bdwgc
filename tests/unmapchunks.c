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
 * Test the unmapping of free blocks in chunks after a collection, with the
 * client allocating between the chunks: the incremental mode with the manual
 * VDB and a time limit of 10 microseconds.  In each part, the test allocates
 * a region (two objects larger than the most memory unmapped before a chunk
 * can stop, whose unmapping is thus split, and many objects of 1 MiB, each
 * allocated before a keeper, see `KEEPER_BYTES`), frees the region's objects
 * explicitly, runs a full collection, and runs collections with small
 * objects, each freed at once (see `run_round()`), until it sees the
 * unmapping pending.  Then, until the part ends, it allocates a large object
 * in every round (of a size between 128 KiB and 1 MiB, kept in a ring, and
 * freed explicitly at times), so that the allocator takes the free blocks
 * being unmapped and their parts, an object larger than the region's 1 MiB
 * blocks and smaller than its large ones, and an object larger than every
 * block of the region (which may grow the heap).  Every page of each kept
 * object holds its key: memory unmapped while in use reads as zeros or
 * faults.  The unmapping was pending if the unmapped bytes grew (by free
 * blocks of the region or others) in a round in which no finish of a
 * collection ended and the heap did not grow; it must be seen pending in
 * every part, unless the write-barrier checker runs (then the finish runs at
 * once), the build lacks the parallel marker (only with it the unmapping
 * runs in chunks), or the test runs on Windows or with `BSD_TIME` (see
 * `UNMAP_CHUNKS`).  Without the manual VDB mode, memory unmapping or the
 * clock, if `NO_INCREMENTAL` is defined, or on 32-bit Windows (see the
 * `main()` that skips the test), the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"
#include "gc/gc_inline.h"

#include <stdio.h>
#include <stdlib.h>

#if !defined(NO_CLOCK) && !(defined(_WIN32) && !defined(_WIN64))
#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define MiB ((size_t)1 << 20)

/*
 * Whether the unmapping after a collection spans several chunks.  Only
 * with the parallel marker it runs in chunks.  The clock of the collector
 * counts milliseconds on Windows (`GetTickCount64()`), thus a time limit
 * lasts until the next tick there, and the unmapping may end in one chunk.
 * With `BSD_TIME` it reads the user time of the process (`getrusage()`),
 * which leaves out the time that the kernel spends unmapping, so there too
 * the unmapping may end in one chunk.
 */
#  if defined(PARALLEL_MARK) && !defined(_WIN32) && !defined(BSD_TIME)
#    define UNMAP_CHUNKS 1
#  else
#    define UNMAP_CHUNKS 0
#  endif

/* The words of a page that the test writes and checks. */
#  define PAGE_WORDS (4096 / sizeof(GC_word))

/*
 * The region: `N_SPLIT` objects of `SPLIT_BYTES` (more than the 32 MiB
 * unmapped at most before a chunk can stop), then objects of 1 MiB.
 */
#  define N_REGION 40
#  define N_SPLIT 2
#  define SPLIT_BYTES (40 * MiB)
#  define REGION_OBJ_BYTES MiB

/*
 * The size of a keeper, kept until the end of the part: more than half of
 * the largest block size (64 KiB), so that each keeper is a large object
 * with blocks of its own.  The allocator takes most keepers from smaller
 * free blocks elsewhere: in the first part most 1 MiB objects of the region
 * lie next to another one, and the blocks of such objects merge into larger
 * free blocks when they are freed.
 */
#  define KEEPER_BYTES (33 * 1024)

#  define N_PARTS 6

/* The large objects kept in the ring. */
#  define N_RING 8

/*
 * The rounds of a part at most, the collections to wait for the unmapping at
 * most, and the collections while it is busy.  The waiting rounds free what
 * they allocate, of which the collector counts only an eighth toward the
 * next collection (see `GC_adj_bytes_allocd()`): a collection takes
 * thousands of rounds then, and more with more data in use.
 */
#  define MAX_ROUNDS 20000000UL
#  define MAX_WAIT_COLLECTIONS 40
#  define N_COLLECTIONS 8

struct kept {
  GC_word *p;
  size_t bytes;
  GC_word key;
};

/*
 * Live data kept for the whole test, so that the collections while the
 * region ages are not much more frequent than the chunks of the unmapping.
 */
#  define BALLAST_BYTES (32 * MiB)
static void *volatile ballast;

static GC_word *volatile region[N_REGION];
static GC_word *volatile keepers[N_REGION];
static struct kept ring[N_RING];
static struct kept merged, grown;
static GC_word next_key = 1;

/* The finishes of collections that ended.  Updated holding the lock. */
static volatile unsigned long n_finish_ends;

static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_RECLAIM_END == e)
    n_finish_ends++;
}

/* Allocate an object of `bytes` bytes and write `key` to every page. */
static void
new_kept(struct kept *k, size_t bytes)
{
  size_t i;

  k->p = (GC_word *)GC_malloc_atomic(bytes);
  CHECK(k->p != NULL);
  k->bytes = bytes;
  k->key = next_key++;
  for (i = 0; i < bytes / sizeof(GC_word); i += PAGE_WORDS)
    k->p[i] = k->key;
  k->p[bytes / sizeof(GC_word) - 1] = k->key;
}

static void
check_kept(const struct kept *k, const char *what)
{
  size_t i;

  if (NULL == k->p)
    return;
  for (i = 0; i < k->bytes / sizeof(GC_word); i += PAGE_WORDS) {
    if (k->p[i] != k->key) {
      fprintf(stderr, "Page %lu of the %s object %lu overwritten\n",
              (unsigned long)(i / PAGE_WORDS), what, (unsigned long)k->key);
      exit(1);
    }
  }
  if (k->p[k->bytes / sizeof(GC_word) - 1] != k->key) {
    fprintf(stderr, "End of the %s object %lu overwritten\n", what,
            (unsigned long)k->key);
    exit(1);
  }
}

static void
check_all(void)
{
  int i;

  for (i = 0; i < N_RING; i++)
    check_kept(&ring[i], "ring");
  check_kept(&merged, "merged");
  check_kept(&grown, "grown");
}

static void
drop(struct kept *k)
{
  k->p = NULL;
  k->bytes = 0;
}

/*
 * Run a round of a part.  If `busy`, then allocate the large objects.
 * Otherwise free each small object at once, by `GC_free()` (it is allocated
 * without a debug header), so that the next one reuses it: the waiting
 * rounds refill the free list, and take a block for it, only after the
 * finish of a collection has emptied it.  `GC_free()` puts an object on the
 * global free list, thus the waiting rounds take their objects from there,
 * by `GC_malloc_kind_global()`.  `GC_malloc_atomic()` takes them from there
 * too where no thread-local free list covers their size, but else (e.g. with
 * `GC_TINY_FREELISTS` of 129) from the free list of the thread: the next
 * object would not be the one freed, and the refills of that list could take
 * new blocks.  New blocks would be split off the free blocks of the region
 * in the first part, where the region holds most of the free memory, and the
 * rest of a split block counts as freed by the latest collection: in a slow
 * build, whose collections mark for many rounds, every block of the region
 * could be split before it gets old.  And every refill also runs a chunk of
 * the finish if one is due: in a slow round with refills, every chunk of the
 * unmapping could run in the round in which the finish ends, while a round
 * without refills runs one chunk at most, in `GC_collect_a_little()`.
 * Return whether the unmapping was seen pending.
 */
static int
run_round(unsigned long round, int part, int busy)
{
  unsigned long ends = n_finish_ends;
  size_t heap = GC_get_heap_size() + GC_get_unmapped_bytes();
  size_t unmapped = GC_get_unmapped_bytes();
  int j;

  for (j = 0; j < 32; j++) {
    if (busy) {
      CHECK(GC_MALLOC_ATOMIC(1024) != NULL);
    } else {
      void *p = GC_malloc_kind_global(1024, GC_I_PTRFREE);

      CHECK(p != NULL);
      GC_free(p);
    }
  }
  if (busy) {
    struct kept *k = &ring[round % N_RING];

    if (round % 3 == 0 && k->p != NULL) {
      GC_free(k->p);
      drop(k);
    }
    new_kept(k, (1 + (round + (unsigned long)part) % 8) * (MiB / 8));
    if (NULL == merged.p)
      new_kept(&merged, SPLIT_BYTES - SPLIT_BYTES / 8);
    else if (NULL == grown.p)
      new_kept(&grown, N_SPLIT * SPLIT_BYTES);
  }
  (void)GC_collect_a_little();
  return GC_get_unmapped_bytes() > unmapped && n_finish_ends == ends
         && GC_get_heap_size() + GC_get_unmapped_bytes() == heap;
}

/*
 * Run a part: the region, then rounds until the unmapping is seen
 * pending, then busy rounds until `N_COLLECTIONS` collections have
 * completed.  Return the rounds in which the unmapping was pending.
 */
static unsigned long
run_part(int part)
{
  GC_word start;
  unsigned long round, n_pending = 0;
  int i;

  for (i = 0; i < N_REGION; i++) {
    size_t bytes = i < N_SPLIT ? SPLIT_BYTES : REGION_OBJ_BYTES;
    GC_word *p = (GC_word *)GC_malloc_atomic(bytes);
    size_t j;

    CHECK(p != NULL);
    for (j = 0; j < bytes / sizeof(GC_word); j += PAGE_WORDS)
      p[j] = (GC_word)j;
    region[i] = p;
    keepers[i] = (GC_word *)GC_MALLOC_ATOMIC(KEEPER_BYTES);
    CHECK(keepers[i] != NULL);
  }
  /*
   * Free the region explicitly: a stale pointer could keep an object of
   * it alive through a collection, and the minor collections keep its
   * mark, so it would be freed only by a later full collection, and its
   * blocks would merge with the free blocks of the region next to them,
   * which would then count as freed by that collection.  By `GC_free()`,
   * as the objects of the region and those of `new_kept()` are allocated
   * without a debug header: `GC_FREE()` is `GC_debug_free()` with
   * `GC_DEBUG`, which leaves a collectable object to the collector.
   */
  for (i = 0; i < N_REGION; i++) {
    GC_free((void *)region[i]);
    region[i] = NULL;
  }
  GC_gcollect();

  start = GC_get_gc_no();
  for (round = 1; 0 == n_pending; round++) {
    CHECK(round < MAX_ROUNDS);
    if (GC_get_gc_no() - start > MAX_WAIT_COLLECTIONS) {
#  if UNMAP_CHUNKS
      if (NULL == getenv("GC_WBAR_VERIFY")) {
        fprintf(stderr, "The unmapping was never pending (part %d)\n", part);
        exit(1);
      }
#  endif
      break;
    }
    if (run_round(round, part, 0))
      n_pending++;
  }
  start = GC_get_gc_no();
  for (; GC_get_gc_no() - start < N_COLLECTIONS; round++) {
    CHECK(round < MAX_ROUNDS);
    if (run_round(round, part, 1))
      n_pending++;
    if (round % 16 == 0)
      check_all();
  }
  check_all();
  drop(&merged);
  drop(&grown);
  for (i = 0; i < N_REGION; i++)
    keepers[i] = NULL;
  return n_pending;
}

int
main(void)
{
  struct GC_timeval_s tv;
  unsigned long n_pending = 0;
  int part;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
#  ifndef NO_INCREMENTAL
  GC_enable_incremental();
#  endif
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  CHECK(GC_MALLOC_ATOMIC(MiB) != NULL);
  GC_gcollect_and_unmap();
  if (0 == GC_get_unmapped_bytes()) {
    printf("Memory unmapping is unavailable, test skipped\n");
    return 0;
  }
  tv.tv_ms = 0;
  tv.tv_nsec = 10 * 1000;
  GC_set_time_limit_tv(tv);
  /* Keep the heap small: the region and the kept objects are large. */
  GC_set_alloc_budget_percent(100);
  ballast = GC_MALLOC_ATOMIC(BALLAST_BYTES);
  CHECK(ballast != NULL);
  GC_set_on_collection_event(on_event);
  for (part = 0; part < N_PARTS; part++)
    n_pending += run_part(part);
  printf("SUCCEEDED (the unmapping pending in %lu rounds)\n", n_pending);
  return 0;
}

#else
int
main(void)
{
#  ifdef NO_CLOCK
  printf("The clock is unavailable, test skipped\n");
#  else
  /*
   * The test grows the heap many times, with objects of up to 80 MiB, and
   * each growth takes a heap section, of which a 32-bit Windows build has
   * 512: the collector aborts with "Too many heap sections".
   */
  printf("Too few heap sections on 32-bit Windows, test skipped\n");
#  endif
  return 0;
}
#endif
