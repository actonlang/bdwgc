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
 * an entry per dirty block of small objects that a marker expands with
 * `GC_rescue_block_proc()`: the incremental mode with the manual VDB mode
 * and no time limit.  Without the incremental mode, no mark procedure may
 * be registered for this.  The old objects, marked by a full collection,
 * are of the size of a granule: of the normal kind, in blocks where every
 * other object is dead, of a kind whose descriptor is the length of an
 * object (unlike the normal one with the extra byte), in blocks where runs
 * of 3 live objects alternate with a dead one, and of a typed kind, all
 * live (more than 512 in a block of 16 KiB).  After the full collection,
 * each old object gets a pointer to a new object, with the dirty bit of
 * its block set, and nothing else refers to the new objects.  The next
 * collection, a minor one, must mark every new object, and call the
 * procedure for each block at least.  This runs with the mark phases of
 * one marker (by `GC_set_bytes_per_marker()`), and with all markers.  But
 * first, the procedure is called for a block of each kind with patterns
 * of marked objects (runs, every other one, all, none, the first or the
 * last one), and 3, 4, 63 and 1023 free entries in the mark stack, as
 * a marker would, until no entry of the rest is left.  The entries pushed
 * must cover each marked object once and no other object, in a run per
 * entry only with the descriptor of the length of an object, at most 512
 * entries per call, the entry of the rest at the bottom.  With 0 to 2 free
 * entries, the marked objects (no more than the free entries) must be
 * pushed one by one, with no entry of the rest.  The objects marked before
 * a call for the rest must be pushed by it.  No call may write at or past
 * the limit, nor at or below the top entry.  This test includes the
 * collector source to read the mark bits and the markers of a mark phase,
 * and to call the procedure.  Skipped without the manual VDB mode.
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

/* The calls of the procedure in the collections. */
#  ifdef PARALLEL_MARK
static volatile AO_t n_proc_calls;
#  else
static GC_word n_proc_calls;
#  endif

/* The procedure, counting its calls. */
static struct GC_ms_entry *GC_CALLBACK
counting_proc(word *addr, struct GC_ms_entry *msp, struct GC_ms_entry *msl,
              word env)
{
#  ifdef PARALLEL_MARK
  (void)AO_fetch_and_add1(&n_proc_calls);
#  else
  n_proc_calls++;
#  endif
  return GC_rescue_block_proc(addr, msp, msl, env);
}

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

/* The blocks of the old objects. */
#  define MAX_BLOCKS 64
static struct hblk *blocks[MAX_BLOCKS];
static size_t n_blocks;

/* Add the blocks of the objects in `a` to `blocks`. */
static void
add_blocks(struct old *const *a, size_t n)
{
  size_t i, j;

  for (i = 0; i < n; i++) {
    struct hblk *h = HBLKPTR(a[i]);

    for (j = 0; j < n_blocks && blocks[j] != h; j++) {
      /* Empty. */
    }
    if (j == n_blocks) {
      CHECK(n_blocks < MAX_BLOCKS);
      blocks[n_blocks++] = h;
    }
  }
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
  n_proc_calls = 0;
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
  printf("%s (%u markers): new objects unmarked: %lu, %lu and %lu, %lu calls"
         " of the procedure\n",
         name, last_markers, (unsigned long)n_alt, (unsigned long)n_runs,
         (unsigned long)n_typed, (unsigned long)n_proc_calls);
  CHECK(0 == n_alt && 0 == n_runs && 0 == n_typed);
  CHECK(n_proc_calls >= n_blocks);
}

/* The entries below the top entry of a call, and the entries past it. */
#  define N_BELOW 2
#  define N_GUARD 4

/* The most entries from the top entry of a call to the limit. */
#  define MAX_ROOM 1024

/* The mark stack of the calls. */
static mse ms[N_BELOW + MAX_ROOM + N_GUARD];

/*
 * The objects marked in the block of the calls, the entries that cover
 * each object, and the entries pushed.
 */
static char marked[HBLKSIZE / GC_GRANULE_BYTES];
static unsigned covers[HBLKSIZE / GC_GRANULE_BYTES];
static size_t n_entries;

/* The calls of the procedure made by the test itself. */
static size_t n_calls;

/* Set the mark bits of the first `n` objects of `hhdr` from `marked`. */
static void
apply_marks(hdr *hhdr, size_t n)
{
  size_t sz = hhdr->hb_sz;
  size_t i;

  for (i = 0; i < n; i++) {
    if (marked[i]) {
      set_mark_bit_from_hdr(hhdr, i * MARK_BIT_OFFSET(sz));
    } else {
      clear_mark_bit_from_hdr(hhdr, i * MARK_BIT_OFFSET(sz));
    }
  }
}

/* Count the objects that the entry `e` covers. */
static void
cover(struct hblk *h, const hdr *hhdr, const mse *e)
{
  size_t sz = hhdr->hb_sz;
  size_t off, len, i;

  CHECK(ADDR_GE(e->mse_start, h->hb_body));
  off = (size_t)(e->mse_start - h->hb_body);
  CHECK(off % sz == 0);
  if (hhdr->hb_descr == (sz | GC_DS_LENGTH)) {
    CHECK((e->mse_descr & GC_DS_TAGS) == GC_DS_LENGTH);
    len = (size_t)e->mse_descr;
    CHECK(len >= sz && len % sz == 0);
  } else {
    CHECK(e->mse_descr == hhdr->hb_descr);
    len = sz;
  }
  CHECK(off + len <= HBLK_OBJS(sz) * sz);
  for (i = off / sz; i < (off + len) / sz; i++)
    covers[i]++;
  n_entries++;
}

static int
is_guard(const mse *e)
{
  return NULL == e->mse_start && GC_WORD_MAX == e->mse_descr;
}

/*
 * Call the procedure for the block `h` from object `env` with the top
 * entry `room` entries below the limit (thus with `room - 1` free entries),
 * as a marker that took the entry would, and count the objects the entries
 * pushed cover.  Return the object of the entry of the rest, or the number
 * of objects of the block if there is none.
 */
static size_t
call_proc(struct hblk *h, size_t room, size_t env)
{
  const hdr *hhdr = HDR(h);
  size_t n = HBLK_OBJS(hhdr->hb_sz);
  mse *msp = ms + N_BELOW - 1;
  mse *msl = msp + room;
  mse *top, *e;
  size_t next = n;
  size_t pushed = 0;

  CHECK(room <= MAX_ROOM);
  for (e = ms; e < ms + sizeof(ms) / sizeof(ms[0]); e++) {
    e->mse_start = NULL;
    e->mse_descr = GC_WORD_MAX;
  }
  top = GC_rescue_block_proc((word *)h, msp, msl, (word)env);
  n_calls++;
  for (e = ms; e <= msp; e++)
    CHECK(is_guard(e));
  for (e = msl; e < msl + N_GUARD; e++)
    CHECK(is_guard(e));
  CHECK(ADDR_GE((ptr_t)top, (ptr_t)msp) && ADDR_LT((ptr_t)top, (ptr_t)msl));
  for (e = msp + 1; ADDR_GE((ptr_t)top, (ptr_t)e); e++) {
    if ((e->mse_descr & GC_DS_TAGS) == GC_DS_PROC) {
      CHECK(e == msp + 1 && room >= 4);
      CHECK(e->mse_start == (ptr_t)h);
      next
          = (size_t)(e->mse_descr >> (GC_DS_TAG_BITS + GC_LOG_MAX_MARK_PROCS));
      CHECK(e->mse_descr == GC_MAKE_PROC(GC_rescue_proc_index, next));
      CHECK(next > env && next < n);
    } else {
      cover(h, hhdr, e);
      pushed++;
    }
  }
  if (room >= 4)
    CHECK(pushed <= RESCUE_MAX_ENTRIES && pushed <= room - 3);
  return next;
}

/* Call the procedure from object `env` on until no entry of the rest. */
static void
expand(struct hblk *h, size_t room, size_t env)
{
  size_t n = HBLK_OBJS(HDR(h)->hb_sz);

  while (env < n)
    env = call_proc(h, room, env);
}

static void
reset_covers(void)
{
  memset(covers, 0, sizeof(covers));
  n_entries = 0;
}

/*
 * Check that the entries cover the first `n` objects as `marked`, and
 * that their number is `expected`.
 */
static void
check_covers(size_t n, size_t expected)
{
  size_t i;

  for (i = 0; i < n; i++)
    CHECK(covers[i] == (unsigned)marked[i]);
  CHECK(n_entries == expected);
}

/* The marked objects of the first `n`, and the runs of marked objects. */
static size_t
count_marked(size_t n, size_t *n_runs)
{
  size_t i;
  size_t r = 0;

  *n_runs = 0;
  for (i = 0; i < n; i++) {
    if (marked[i]) {
      r++;
      if (0 == i || !marked[i - 1])
        (*n_runs)++;
    }
  }
  return r;
}

/*
 * The patterns of marked objects: runs of 1 to 7 objects, 1 or 2 objects
 * apart, every other object, all, none, the first one, the last one.
 */
enum pattern { RUNS, EVERY_OTHER, ALL, NONE, FIRST, LAST, N_PATTERNS };

/* Set `marked` to the pattern `k` for the first `n` objects. */
static void
set_pattern(enum pattern k, size_t n)
{
  size_t i, run, gap;

  memset(marked, 0, sizeof(marked));
  switch (k) {
  case RUNS:
    for (i = 0, run = 1, gap = 1; i < n; run = run % 7 + 1, gap = 3 - gap) {
      size_t j;

      for (j = 0; j < run && i < n; j++)
        marked[i++] = 1;
      i += gap;
    }
    break;
  case EVERY_OTHER:
    for (i = 0; i < n; i += 2)
      marked[i] = 1;
    break;
  case ALL:
    memset(marked, 1, n);
    break;
  case FIRST:
    marked[0] = 1;
    break;
  case LAST:
    marked[n - 1] = 1;
    break;
  default:
    break;
  }
}

/* Call the procedure for the block `h`, of the size of a granule. */
static void
check_proc(struct hblk *h)
{
  static const size_t rooms[] = { 4, 5, 64, MAX_ROOM };
  hdr *hhdr = HDR(h);
  size_t n = HBLK_OBJS(hhdr->hb_sz);
  GC_bool by_length = hhdr->hb_descr == (hhdr->hb_sz | GC_DS_LENGTH);
#  ifdef USE_MARK_BYTES
  char saved[HB_MARKS_SZ];
#  else
  word saved[HB_MARKS_SZ];
#  endif
  size_t i, next, n_marked, n_runs;
  int k;

  n_calls = 0;
  CHECK(n <= sizeof(marked));
  for (i = 0; i < HB_MARKS_SZ; i++)
    saved[i] = hhdr->hb_marks[i];
  for (k = 0; k < N_PATTERNS; k++) {
    set_pattern((enum pattern)k, n);
    apply_marks(hhdr, n);
    n_marked = count_marked(n, &n_runs);
    for (i = 0; i < sizeof(rooms) / sizeof(rooms[0]); i++) {
      reset_covers();
      expand(h, rooms[i], 0);
      check_covers(n, by_length ? n_runs : n_marked);
    }
  }

  /* With fewer than 3 free entries, objects are pushed one by one. */
  set_pattern(NONE, n);
  marked[5] = 1;
  marked[6] = 1;
  apply_marks(hhdr, n);
  reset_covers();
  CHECK(call_proc(h, 3, 0) == n);
  check_covers(n, 2);
  set_pattern(LAST, n);
  apply_marks(hhdr, n);
  reset_covers();
  CHECK(call_proc(h, 2, 0) == n);
  check_covers(n, 1);
  set_pattern(NONE, n);
  apply_marks(hhdr, n);
  reset_covers();
  CHECK(call_proc(h, 1, 0) == n);
  check_covers(n, 0);

  /* The objects marked before a call for the rest are pushed by it. */
  set_pattern(EVERY_OTHER, n);
  apply_marks(hhdr, n);
  reset_covers();
  next = call_proc(h, 5, 0);
  CHECK(next < n);
  for (i = next; i < n; i++)
    marked[i] = 1;
  apply_marks(hhdr, n);
  expand(h, 5, next);
  for (i = 0; i < n; i++)
    CHECK(covers[i] == (unsigned)marked[i]);

  for (i = 0; i < HB_MARKS_SZ; i++)
    hhdr->hb_marks[i] = saved[i];
  printf("%lu calls of the procedure for a block of %lu objects of kind %u"
         " (%s)\n",
         (unsigned long)n_calls, (unsigned long)n, (unsigned)hhdr->hb_obj_kind,
         by_length ? "runs" : "one by one");
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
  if (!GC_is_incremental_mode()) {
    unsigned n_procs;

    LOCK();
    n_procs = GC_n_mark_procs;
    UNLOCK();
    GC_gcollect();
    LOCK();
    CHECK(GC_rescue_proc_index == GC_MAX_MARK_PROCS);
    CHECK(GC_n_mark_procs == n_procs);
    UNLOCK();
  }
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
  add_blocks(alt, sizeof(alt) / sizeof(alt[0]));
  add_blocks(runs, sizeof(runs) / sizeof(runs[0]));
  add_blocks(typed, sizeof(typed) / sizeof(typed[0]));
  GC_set_on_collection_event(on_event);
  GC_gcollect();
  CHECK(is_marked(alt[0]) && is_marked(runs[0]) && is_marked(typed[0]));
  LOCK();
  CHECK(GC_rescue_proc_index < GC_MAX_MARK_PROCS);
  CHECK(GC_mark_procs[GC_rescue_proc_index] == GC_rescue_block_proc);
  GC_mark_procs[GC_rescue_proc_index] = counting_proc;
  check_proc(HBLKPTR(alt[0]));
  check_proc(HBLKPTR(runs[0]));
  check_proc(HBLKPTR(typed[0]));
  UNLOCK();
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
