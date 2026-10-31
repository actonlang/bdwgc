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
 * Test the pass over the dirty blocks of a minor collection, which pushes
 * the marked objects of the dirty blocks: the incremental mode with the
 * manual VDB mode and no time limit.  The old objects, marked by a full
 * collection, are of the size of a granule: of the normal kind, in blocks
 * where every other object is dead, of a kind whose descriptor is the
 * length of an object (unlike the normal one with the extra byte), in
 * blocks where runs of 3 live objects alternate with a dead one, and of
 * a typed kind, all live (more than 512 in a block of 16 KiB).  After the
 * full collection, each old object gets a pointer to a new object, with
 * the dirty bit of its block set, and nothing else refers to the new
 * objects.  The next collection, a minor one, must mark every new object.
 * This runs with the mark phases of one marker (by
 * `GC_set_bytes_per_marker()`), and with all markers.  This test includes
 * the collector source to read the mark bits and the markers of a mark
 * phase.  Skipped without the manual VDB mode.
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
#include "../extra/gc.c"
#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
#  pragma GCC diagnostic pop
#endif

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#if !defined(NO_MANUAL_VDB) && !defined(NO_INCREMENTAL)

/* The old objects of each pattern: 3 blocks of objects of a granule. */
#  define N_OLD (3 * HBLKSIZE / GC_GRANULE_BYTES)

/* An old object, of a granule (with the extra byte, if any). */
struct old {
  struct young *p;
};

struct young {
  GC_word tag[5];
};

/*
 * The live old objects of the normal kind (every other one), of the kind
 * of a length descriptor (runs of 3), and of the typed kind.
 */
static struct old *alt[N_OLD / 2];
static struct old *runs[N_OLD / 4 * 3];
static struct old *typed[N_OLD];

/* Whether a collection was full, and the markers of its last mark phase. */
static GC_bool last_full;
static unsigned last_markers;

/* Called with the allocator lock held. */
static void GC_CALLBACK
on_event(GC_EventType e)
{
  if (GC_EVENT_MARK_END == e) {
    last_full = GC_is_full_gc;
#  ifdef PARALLEL_MARK
    last_markers = GC_phase_markers;
#  else
    last_markers = 1;
#  endif
  }
}

static struct old *
new_old(int kind)
{
  struct old *o = (struct old *)GC_generic_malloc(sizeof(struct old), kind);

  CHECK(o != NULL);
  o->p = NULL;
  return o;
}

/* Whether the object `p` is marked, false if its block was freed. */
static int
is_marked(const void *p)
{
  const hdr *hhdr;
  int r;

  LOCK();
  hhdr = HDR(p);
  r = !IS_FORWARDING_ADDR_OR_NIL(hhdr) && !HBLK_IS_FREE(hhdr)
      && GC_is_marked(p);
  UNLOCK();
  return r;
}

/*
 * Store a pointer to a new object into `o`, and dirty the block of `o`.
 * The new object is not from a thread-local free list, whose objects are
 * marked by every collection, thus it is unmarked.
 */
static void
store_young(struct old *o)
{
  struct young *y
      = (struct young *)GC_generic_malloc(sizeof(struct young), NORMAL);

  CHECK(y != NULL);
  CHECK(!is_marked(y));
  GC_PTR_STORE_AND_DIRTY(&o->p, y);
}

static GC_ATTR_NOINLINE void
store_all(void)
{
  size_t i;

  for (i = 0; i < sizeof(alt) / sizeof(alt[0]); i++)
    store_young(alt[i]);
  for (i = 0; i < sizeof(runs) / sizeof(runs[0]); i++)
    store_young(runs[i]);
  for (i = 0; i < sizeof(typed) / sizeof(typed[0]); i++)
    store_young(typed[i]);
}

/* Overwrite the stack below the caller, to drop stale pointers. */
static GC_ATTR_NOINLINE void
clear_stack(void)
{
  volatile char buf[16384];

  memset((char *)buf, 0, sizeof(buf));
  GC_noop1((GC_word)(GC_uintptr_t)buf);
}

/* Count the old objects in `a` whose new object is not marked. */
static size_t
count_unmarked(struct old *const *a, size_t n)
{
  size_t i;
  size_t r = 0;

  for (i = 0; i < n; i++) {
    if (!is_marked(a[i]->p))
      r++;
  }
  return r;
}

/*
 * Store new objects into the old ones, and collect with mark phases of
 * `bytes_per_marker` (0 means all the markers).
 */
static void
run(size_t bytes_per_marker, const char *name)
{
  size_t n_alt, n_runs, n_typed;
  GC_word gc_no;

  GC_set_bytes_per_marker(bytes_per_marker);
  gc_no = GC_get_gc_no();
  store_all();
  clear_stack();
  LOCK();
  GC_need_full_gc = FALSE;
  UNLOCK();
  GC_start_incremental_collection();
  CHECK(GC_get_gc_no() == gc_no + 1);
  LOCK();
  CHECK(!last_full);
#  ifdef PARALLEL_MARK
  CHECK(last_markers
        == (0 == bytes_per_marker ? (unsigned)GC_markers_m1 + 1 : 1U));
#  endif
  UNLOCK();
  n_alt = count_unmarked(alt, sizeof(alt) / sizeof(alt[0]));
  n_runs = count_unmarked(runs, sizeof(runs) / sizeof(runs[0]));
  n_typed = count_unmarked(typed, sizeof(typed) / sizeof(typed[0]));
  printf("%s (%u markers): new objects unmarked: %lu, %lu and %lu\n", name,
         last_markers, (unsigned long)n_alt, (unsigned long)n_runs,
         (unsigned long)n_typed);
  CHECK(0 == n_alt && 0 == n_runs && 0 == n_typed);
}

int
main(void)
{
  GC_word bm = 1;
  GC_descr d;
  int length_kind;
  size_t i;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  /* The marker threads start with the first thread otherwise. */
  GC_start_mark_threads();
  GC_enable_incremental();
  if (!GC_is_incremental_mode()
      || (GC_get_actual_vdb() & GC_VDB_MANUAL) == 0) {
    printf("The manual VDB mode is unavailable, test skipped\n");
    return 0;
  }
  GC_set_time_limit(GC_TIME_UNLIMITED);
  GC_set_full_freq(100000);
  /* No collection that follows a heap growth. */
  CHECK(GC_expand_hp(16 << 20));
  d = GC_make_descriptor(&bm, 1);
  length_kind = (int)GC_new_kind(GC_new_free_list(), GC_DS_LENGTH, 1, 1);
  for (i = 0; i < N_OLD; i++) {
    struct old *o = new_old(NORMAL);

    if (i % 2 == 0)
      alt[i / 2] = o;
  }
  for (i = 0; i < N_OLD; i++) {
    struct old *o = new_old(length_kind);

    if (i % 4 != 3)
      runs[i / 4 * 3 + i % 4] = o;
  }
  for (i = 0; i < N_OLD; i++) {
    typed[i] = (struct old *)GC_malloc_explicitly_typed(sizeof(struct old), d);
    CHECK(typed[i] != NULL);
  }
  GC_set_on_collection_event(on_event);
  GC_gcollect();
  CHECK(is_marked(alt[0]) && is_marked(runs[0]) && is_marked(typed[0]));
  run(~(size_t)0, "one marker");
  run(0, "all markers");
  GC_set_on_collection_event(0);
  printf("SUCCEEDED\n");
  return 0;
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
