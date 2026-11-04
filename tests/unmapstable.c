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
 * Test that the age of a free block which may be unmapped counts full
 * collections only, and that the unmapping keeps mapped the memory in use
 * at the end of the marking of a minor collection while the full collection
 * after it is one of those counted: the incremental mode with the manual
 * VDB and a time limit of 10 microseconds.  In each part, the test
 * allocates a region of 1 MiB objects, each followed by a kept one (see
 * `kept`), frees the region in one of three ways (see `FREED_BY_FULL`), and
 * runs minor collections, with a stable working set and small objects that
 * die at once (see `run_collection()`), until a full collection starts: the
 * region must not be unmapped meanwhile.  Then it runs full collections,
 * without allocating in between: the region must be unmapped, but not
 * before the full collections of the unmapping threshold
 * (`GC_UNMAP_THRESHOLD`, 7 by default, and 16 at most, the full collections
 * counted) have completed since it was freed, nor before one more if a
 * minor collection was the last one to find it in use.  Without the manual
 * VDB mode, memory unmapping or the clock, if every collection is full, if
 * `NO_INCREMENTAL` is defined, or with a threshold above `MAX_THRESHOLD`,
 * the test is skipped.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gc.h"
#include "gc/gc_inline.h"
#include "gc/gc_mark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NO_CLOCK
#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define MiB ((size_t)1 << 20)

/* The objects of a region. */
#  define N_REGION 48

/*
 * The parts, by how the region is freed: by the finish of a full
 * collection (its blocks are in use at the end of the marking of that
 * collection); explicitly, allocated and freed while the collections are
 * disabled, thus never in use at the end of the marking of a collection;
 * explicitly, after a minor collection found it in use.  Only the second
 * part fails if the age counts every collection and the memory in use stays
 * mapped, and only the third one if the age counts full collections and
 * nothing stays mapped.
 */
#  define FREED_BY_FULL 0
#  define FREED_UNSEEN 1
#  define FREED_AFTER_MINOR 2
#  define N_PARTS 3

/*
 * The unmapping threshold of the collector by default, the most full
 * collections that the collector counts in the age of a free block, and
 * the highest threshold tested: a block must also have been free for that
 * many collections in all, which the full collections run at most (see
 * `MAX_FULL`) must hold.
 */
#  ifdef MUNMAP_THRESHOLD
#    define DEFAULT_THRESHOLD MUNMAP_THRESHOLD
#  else
#    define DEFAULT_THRESHOLD 7
#  endif
#  define MAX_FULL_THRESHOLD 16
#  define MAX_THRESHOLD 32

/* The minor collections at most, and the full collections at most. */
#  define N_MINOR 40
#  define MAX_FULL 40

/* The small objects of the working set. */
#  define N_RING 4096

/*
 * The objects of the region, those kept until the end, and the working set
 * (see `N_RING`).  The objects of a region and the kept ones are allocated in
 * turn, so that each free block of the region lies between objects in use, or
 * in a free block left by an earlier region between kept objects.  A free
 * block of the region next to another free block would be merged with it when
 * the latter is freed, and the merged block counts as freed then.
 */
static GC_word *volatile region[N_REGION];
static GC_word *volatile kept[N_PARTS][N_REGION];
static void *volatile ring[N_RING];

/*
 * The full collections started, and the collection number of the latest
 * one.  Updated holding the lock.
 */
static volatile unsigned long n_full_starts;
static volatile GC_word last_full_gc_no;

static void GC_CALLBACK
on_full_start(void)
{
  n_full_starts++;
  last_full_gc_no = GC_get_gc_no() + 1;
}

/*
 * Allocate small objects until a collection completes, each freed at once
 * by `GC_free()` (it is allocated without a debug header), so that the
 * next one reuses it.  `GC_free()` puts an object on the global free list,
 * thus the objects are taken from there, by `GC_malloc_kind_global()`
 * (from a thread-local free list, the next object would not be the one
 * freed).  The allocation takes a new block only after the finish of a
 * collection has emptied the free list, and such blocks are split off the
 * free blocks, possibly those of the region, and the rest of a split block
 * counts as freed by the latest collection.  An allocation from a free
 * list that is not empty runs no step and starts no collection, which
 * `GC_collect_a_little()` does when one is due.  Nothing survives a minor
 * collection, thus the bytes in use do not grow between full collections,
 * and `GC_GEN_FULL_LIVE_PCT` does not make a full collection due.
 * Return whether that collection is a full one.
 */
static int
run_collection(void)
{
  GC_word start = GC_get_gc_no();

  while (GC_get_gc_no() == start) {
    int i;

    for (i = 0; i < 32; i++) {
      void *p = GC_malloc_kind_global(1024, GC_I_PTRFREE);

      CHECK(p != NULL);
      GC_free(p);
    }
    (void)GC_collect_a_little();
  }
  return last_full_gc_no == GC_get_gc_no();
}

/*
 * Free the objects of the region by `GC_free()`: they are allocated
 * without a debug header (`GC_FREE()` is `GC_debug_free()` with
 * `GC_DEBUG`, which leaves a collectable object to the collector).
 */
static void
free_region(void)
{
  int i;

  for (i = 0; i < N_REGION; i++) {
    GC_free((void *)region[i]);
    region[i] = NULL;
  }
}

/*
 * Run a part: allocate the region, free it as `part` says, and check its
 * unmapping, with `min_full` the number of full collections of the threshold.
 */
static void
run_part(int part, unsigned long min_full)
{
  GC_warn_proc warn_proc = 0;
  size_t unmapped, unmapped_at_free = 0;
  unsigned long full_start, n_full, n_full_before = 0;
  int i, n_minor;

  /* Complete the collection in progress and its unmapping. */
  GC_gcollect();
  if (FREED_UNSEEN == part) {
    GC_disable();
    /* Silence the warning that the heap grew while collections were off. */
    warn_proc = GC_get_warn_proc();
    GC_set_warn_proc(GC_ignore_warn_proc);
  }
  for (i = 0; i < N_REGION; i++) {
    region[i] = (GC_word *)GC_malloc_atomic(MiB);
    CHECK(region[i] != NULL);
    region[i][0] = (GC_word)i;
    kept[part][i] = (GC_word *)GC_malloc_atomic(MiB);
    CHECK(kept[part][i] != NULL);
  }
  if (FREED_BY_FULL == part) {
    /*
     * `GC_gcollect()` completes the collection in progress before its own,
     * and that one would free the region if it were dropped first.
     */
    GC_gcollect();
    for (i = 0; i < N_REGION; i++)
      region[i] = NULL;
    GC_gcollect();
  } else if (FREED_UNSEEN == part) {
    free_region();
    GC_enable();
    GC_set_warn_proc(warn_proc);
    /*
     * A full collection would start after a few minor ones, for the kept
     * objects allocated since the latest one (see `GC_need_full_gc`): run
     * it before the minor collections, as the first full collection since
     * the region was freed.  It unmaps the region if the threshold is 1.
     */
    unmapped_at_free = GC_get_unmapped_bytes();
    GC_gcollect();
    n_full_before = 1;
  } else {
    /*
     * The region is freed before the next collection completes its
     * marking, after a minor collection completed it.  A full one, where
     * the region was in use the latest time, would leave its peak out of
     * the full collections counted after the region is freed.
     */
    for (i = 0; run_collection(); i++)
      CHECK(i < N_MINOR);
    free_region();
    min_full++;
  }
  unmapped = GC_get_unmapped_bytes();
  if (0 == n_full_before)
    unmapped_at_free = unmapped;
  full_start = n_full_starts;

  /* Minor collections: the region must not be unmapped. */
  for (n_minor = 0; n_minor < N_MINOR; n_minor++) {
    (void)run_collection();
    if (n_full_starts != full_start)
      break;
    if (GC_get_unmapped_bytes() >= unmapped + MiB) {
      fprintf(stderr, "Part %d: %lu KiB unmapped by %d minor collections\n",
              part,
              (unsigned long)((GC_get_unmapped_bytes() - unmapped) >> 10),
              n_minor + 1);
      exit(1);
    }
  }

  /*
   * Full collections, without allocation in between (which would split
   * blocks of the region, and the rest of a split block counts as freed by
   * the latest collection): the region is unmapped.  `GC_gcollect()`
   * completes the collection in progress first, which may be the full one
   * that ended the minor collections.
   */
  for (i = 0;
       i < MAX_FULL && GC_get_unmapped_bytes() < unmapped_at_free + 32 * MiB;
       i++)
    GC_gcollect();
  n_full = n_full_before + n_full_starts - full_start;
  printf("Part %d: %d minor collections, %lu full ones, %lu KiB unmapped\n",
         part, n_minor, n_full,
         (unsigned long)((GC_get_unmapped_bytes() - unmapped_at_free) >> 10));
  fflush(stdout);
  CHECK(GC_get_unmapped_bytes() >= unmapped_at_free + 32 * MiB);
  CHECK(n_full >= min_full);
}

int
main(void)
{
  struct GC_timeval_s tv;
  unsigned long min_full = DEFAULT_THRESHOLD;
  int i;
  const char *threshold = getenv("GC_UNMAP_THRESHOLD");

  /* Take the threshold from the environment as the collector does. */
  if (threshold != NULL) {
    if (0 == strcmp(threshold, "0"))
      min_full = 0;
    else if (atoi(threshold) > 0)
      min_full = (unsigned long)atoi(threshold);
  }
  if (0 == min_full) {
    printf("Memory unmapping is disabled, test skipped\n");
    return 0;
  }
  if (min_full > MAX_THRESHOLD) {
    printf("The unmapping threshold is above %d, test skipped\n",
           MAX_THRESHOLD);
    return 0;
  }
  if (min_full > MAX_FULL_THRESHOLD)
    min_full = MAX_FULL_THRESHOLD;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
#  ifndef NO_INCREMENTAL
  GC_enable_incremental();
#  endif
  if ((GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  if (0 == GC_get_full_freq()) {
    printf("Every collection is full, test skipped\n");
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
  GC_set_start_callback(on_full_start);

  /* The working set, with its blocks, before the regions. */
  for (i = 0; i < N_RING; i++) {
    ring[i] = GC_MALLOC(64);
    CHECK(ring[i] != NULL);
  }
  for (i = 0; i < 8; i++)
    (void)run_collection();
  for (i = 0; i < N_PARTS; i++)
    run_part(i, min_full);
  printf("SUCCEEDED\n");
  return 0;
}

#else
int
main(void)
{
  printf("The clock is unavailable, test skipped\n");
  return 0;
}
#endif
