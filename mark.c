/*
 * Copyright 1988, 1989 Hans-J. Boehm, Alan J. Demers
 * Copyright (c) 1991-1995 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 2000 by Hewlett-Packard Company.  All rights reserved.
 * Copyright (c) 2008-2026 Ivan Maidanski
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

#include "private/gc_pmark.h"

/*
 * Make arguments appear live to compiler.  Put here to minimize the
 * risk of inlining.  Used to minimize junk left in registers.
 */
GC_ATTR_NOINLINE
void
GC_noop6(word arg1, word arg2, word arg3, word arg4, word arg5, word arg6)
{
  UNUSED_ARG(arg1);
  UNUSED_ARG(arg2);
  UNUSED_ARG(arg3);
  UNUSED_ARG(arg4);
  UNUSED_ARG(arg5);
  UNUSED_ARG(arg6);
  /* Avoid `GC_noop6` calls to be optimized away. */
#if defined(AO_HAVE_compiler_barrier) && !defined(BASE_ATOMIC_OPS_EMULATED)
  AO_compiler_barrier(); /*< to serve as a special side-effect */
#else
  GC_noop1(0);
#endif
}

GC_API void GC_CALL
GC_noop1(GC_word x)
{
#if defined(AO_HAVE_store) && defined(THREAD_SANITIZER)
  AO_store(&GC_noop_sink, (AO_t)x);
#else
  GC_noop_sink = x;
#endif
}

GC_API void GC_CALL
GC_noop1_ptr(volatile void *p)
{
#if CPP_PTRSZ > CPP_WORDSZ || defined(CPPCHECK)
#  if defined(AO_HAVE_store) && defined(THREAD_SANITIZER)
  GC_cptr_store(&GC_noop_sink_ptr, (ptr_t)CAST_AWAY_VOLATILE_PVOID(p));
#  elif defined(CPPCHECK)
  GC_noop_sink_ptr = (ptr_t)(/* no volatile */ void *)p;
#  else
  GC_noop_sink_ptr = (ptr_t)CAST_AWAY_VOLATILE_PVOID(p);
#  endif
#else
  GC_noop1(ADDR(p));
#endif
}

/*
 * Initialize `GC_obj_kinds` properly and standard free lists properly.
 * This must be done statically since they may be accessed before
 * `GC_init` is called.  It is done here, since we need to deal with
 * mark descriptors.  Note: `GC_obj_kinds[NORMAL].ok_descriptor` is
 * adjusted in `GC_init()` for `EXTRA_BYTES`.
 */
GC_INNER struct obj_kind GC_obj_kinds[MAXOBJKINDS] = {
  /* `PTRFREE` */
  { &GC_aobjfreelist[0], 0 /*< filled in dynamically */,
    /* `0 |` */ GC_DS_LENGTH, FALSE,
    FALSE
        /*, */ OK_DISCLAIM_INITZ },
  /* `NORMAL` */
  { &GC_objfreelist[0], 0,
    /* `0 |` */ GC_DS_LENGTH, TRUE /*< add length to descriptor template */,
    TRUE
        /*, */ OK_DISCLAIM_INITZ },
  /* `UNCOLLECTABLE` */
  { &GC_uobjfreelist[0], 0,
    /* `0 |` */ GC_DS_LENGTH, TRUE /*< add length to descriptor template */,
    TRUE
        /*, */ OK_DISCLAIM_INITZ },
#ifdef GC_ATOMIC_UNCOLLECTABLE
  /* `AUNCOLLECTABLE` */
  { &GC_auobjfreelist[0], 0,
    /* `0 |` */ GC_DS_LENGTH, FALSE,
    FALSE
        /*, */ OK_DISCLAIM_INITZ },
#endif
};

#ifndef GC_NO_DEINIT
/* Note: keep this close to `GC_obj_kinds` definition. */
GC_INNER void
GC_reset_obj_kinds(void)
{
  unsigned i;

  for (i = 0; i < GC_N_KINDS_INITIAL_VALUE; i++)
    GC_obj_kinds[i].ok_reclaim_list = NULL;
  GC_obj_kinds[PTRFREE].ok_freelist = &GC_aobjfreelist[0];
  GC_obj_kinds[NORMAL].ok_freelist = &GC_objfreelist[0];
  GC_obj_kinds[UNCOLLECTABLE].ok_freelist = &GC_uobjfreelist[0];
#  ifdef GC_ATOMIC_UNCOLLECTABLE
  GC_obj_kinds[AUNCOLLECTABLE].ok_freelist = &GC_auobjfreelist[0];
#  endif
  GC_obj_kinds[NORMAL].ok_descriptor = GC_DS_LENGTH;
  GC_n_kinds = GC_N_KINDS_INITIAL_VALUE;
}
#endif

#ifndef INITIAL_MARK_STACK_SIZE
/*
 * `INITIAL_MARK_STACK_SIZE * sizeof(mse)` should be a multiple of `HBLKSIZE`.
 * The incremental collector actually likes a larger size, since it wants to
 * push all marked dirty objects before marking anything new.
 * Currently we let it grow dynamically.
 */
#  define INITIAL_MARK_STACK_SIZE (1 * HBLKSIZE)
#endif

#ifdef PARALLEL_MARK
#  ifdef LINT2
#    define LOCAL_MARK_STACK_SIZE (HBLKSIZE / 8)
#  else
/*
 * Under normal circumstances, this is big enough to guarantee we do not
 * overflow half of it in a single call to `GC_mark_from`.
 */
#    define LOCAL_MARK_STACK_SIZE HBLKSIZE
#  endif
#endif

#if !defined(GC_DISABLE_INCREMENTAL)
/*
 * The number of dirty pages we marked from, excluding pointer-free pages,
 * etc.  Used for logging only.
 */
STATIC word GC_n_rescuing_pages = 0;

/*
 * Whether a pass over the dirty pages has ended since the previous call
 * of `GC_after_marking()`, and `GC_n_rescuing_pages` at the end of the
 * last such pass, for `GC_after_marking()` to log.  The pass ends inside
 * `GC_mark_some()`, thus maybe with the world stopped.  Protected by the
 * allocator lock.
 */
STATIC GC_bool GC_rescue_pass_ended = FALSE;
STATIC word GC_n_rescued_pages = 0;
#endif

GC_API void GC_CALL
GC_set_pointer_mask(GC_word value)
{
#ifdef DYNAMIC_POINTER_MASK
  GC_ASSERT(value >= 0xff); /*< a simple sanity check */
  GC_pointer_mask = value;
#else
  if (value
#  ifdef POINTER_MASK
      != (word)(POINTER_MASK)
#  else
      != GC_WORD_MAX
#  endif
  ) {
    ABORT("Dynamic pointer mask/shift is unsupported");
  }
#endif
}

GC_API GC_word GC_CALL
GC_get_pointer_mask(void)
{
#ifdef DYNAMIC_POINTER_MASK
  GC_word value = GC_pointer_mask;

  if (0 == value) {
    GC_ASSERT(!GC_is_initialized);
    value = GC_WORD_MAX;
  }
  return value;
#elif defined(POINTER_MASK)
  return POINTER_MASK;
#else
  return GC_WORD_MAX;
#endif
}

GC_API void GC_CALL
GC_set_pointer_shift(unsigned value)
{
#ifdef DYNAMIC_POINTER_MASK
  GC_ASSERT(value < CPP_WORDSZ);
  GC_pointer_shift = (unsigned char)value;
#else
  if (value
#  ifdef POINTER_SHIFT
      != (unsigned)(POINTER_SHIFT)
#  endif
  ) {
    ABORT("Dynamic pointer mask/shift is unsupported");
  }
#endif
}

GC_API unsigned GC_CALL
GC_get_pointer_shift(void)
{
#ifdef DYNAMIC_POINTER_MASK
  return GC_pointer_shift;
#elif defined(POINTER_SHIFT)
  GC_STATIC_ASSERT((unsigned)(POINTER_SHIFT) < CPP_WORDSZ);
  return POINTER_SHIFT;
#else
  return 0;
#endif
}

GC_INNER GC_bool
GC_collection_in_progress(void)
{
  return GC_mark_state != MS_NONE;
}

GC_INNER void
GC_clear_hdr_marks(hdr *hhdr)
{
  size_t last_bit;

#ifdef AO_HAVE_load
  /* Atomic access is used to avoid racing with `GC_realloc`. */
  last_bit = FINAL_MARK_BIT(AO_load((volatile AO_t *)&hhdr->hb_sz));
#else
  /*
   * No race as `GC_realloc` holds the allocator lock while updating
   * `hb_sz` field.
   */
  last_bit = FINAL_MARK_BIT(hhdr->hb_sz);
#endif

  BZERO(CAST_AWAY_VOLATILE_PVOID(hhdr->hb_marks), sizeof(hhdr->hb_marks));
  set_mark_bit_from_hdr(hhdr, last_bit);
  hhdr->hb_n_marks = 0;
}

GC_INNER void
GC_set_hdr_marks(hdr *hhdr)
{
  size_t i;
  size_t sz = hhdr->hb_sz;
  size_t n_marks = FINAL_MARK_BIT(sz);

#ifdef USE_MARK_BYTES
  for (i = 0; i <= n_marks; i += MARK_BIT_OFFSET(sz)) {
    hhdr->hb_marks[i] = 1;
  }
#else
  /*
   * Note that all bits are set even in case of not `MARK_BIT_PER_OBJ`,
   * instead of setting every `n`-th bit where `n` is `MARK_BIT_OFFSET(sz)`.
   *  This is done for a performance reason.
   */
  for (i = 0; i < divWORDSZ(n_marks); ++i) {
    hhdr->hb_marks[i] = GC_WORD_MAX;
  }
  /* Set the remaining bits near the end (plus one bit past the end). */
  hhdr->hb_marks[i] = ((((word)1 << modWORDSZ(n_marks)) - 1) << 1) | 1;
#endif
#ifdef MARK_BIT_PER_OBJ
  hhdr->hb_n_marks = n_marks;
#else
  hhdr->hb_n_marks = HBLK_OBJS(sz);
#endif
}

/* Clear all mark bits associated with the block of `hhdr`. */
static void
clear_marks_for_hdr(hdr *hhdr, void *dummy)
{
  UNUSED_ARG(dummy);
  if (IS_UNCOLLECTABLE(hhdr->hb_obj_kind)) {
    /*
     * Mark bit for these is cleared only once the object is deallocated
     * explicitly.  This either frees the block, or the bit is cleared
     * once the object is on the free list.
     */
    return;
  }
  GC_clear_hdr_marks(hhdr);
}

/* Slow but general routines for setting/clearing/getting mark bits. */

GC_INLINE size_t
GC_get_hhdr_mark_bit_no(hdr **phhdr, const void *p)
{
  const struct hblk *h = HBLKPTR(p);

  *phhdr = HDR(h);
  return MARK_BIT_NO((size_t)((ptr_t)p - (ptr_t)h), (*phhdr)->hb_sz);
}

GC_API void GC_CALL
GC_set_mark_bit(const void *p)
{
  hdr *hhdr;
  size_t bit_no = GC_get_hhdr_mark_bit_no(&hhdr, p);

  if (!mark_bit_from_hdr(hhdr, bit_no)) {
    set_mark_bit_from_hdr(hhdr, bit_no);
    INCR_MARKS(hhdr);
  }
}

GC_API void GC_CALL
GC_clear_mark_bit(const void *p)
{
  hdr *hhdr;
  size_t bit_no = GC_get_hhdr_mark_bit_no(&hhdr, p);

  if (mark_bit_from_hdr(hhdr, bit_no)) {
    size_t n_marks = hhdr->hb_n_marks;

    GC_ASSERT(n_marks != 0);
    clear_mark_bit_from_hdr(hhdr, bit_no);
    n_marks--;
#ifdef PARALLEL_MARK
    /*
     * Do not decrement to zero.  The counts are approximate due to
     * concurrency issues, but we need to ensure that a count of zero
     * implies an empty block.
     */
    if (n_marks != 0 || !GC_parallel)
      hhdr->hb_n_marks = n_marks;
#else
    hhdr->hb_n_marks = n_marks;
#endif
  }
}

GC_API int GC_CALL
GC_is_marked(const void *p)
{
  hdr *hhdr;
  size_t bit_no = GC_get_hhdr_mark_bit_no(&hhdr, p);

  return (int)mark_bit_from_hdr(hhdr, bit_no); /*< 0 or 1 */
}

GC_INNER void
GC_clear_marks(void)
{
  /* The initialization is needed for `GC_push_roots()`. */
  GC_ASSERT(GC_is_initialized);

  GC_apply_to_all_hdrs(clear_marks_for_hdr, NULL);
  GC_objects_are_marked = FALSE;
  GC_mark_state = MS_INVALID;
  GC_scan_ptr = NULL;
}

GC_INNER void
GC_set_valid_ds_mark_obj(const void *p)
{
  hdr *hhdr;
  size_t bit_no = GC_get_hhdr_mark_bit_no(&hhdr, p);

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(!hdr_valid_ds_mark(hhdr->hb_valid_ds_bitmap, bit_no));
  hdr_set_valid_ds_mark(hhdr, bit_no);
}

GC_INNER void
GC_initiate_gc(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_is_initialized);
#ifndef GC_DISABLE_INCREMENTAL
  if (GC_incremental) {
#  ifdef CHECKSUMS
    GC_read_dirty(FALSE);
    GC_check_dirty();
#  else
    GC_read_dirty(GC_mark_state == MS_INVALID);
#  endif
  }
  GC_n_rescuing_pages = 0;
#endif
  if (GC_mark_state == MS_NONE) {
    GC_mark_state = MS_PUSH_RESCUERS;
  } else {
    /* This is really a full collection, and mark bits are invalid. */
    GC_ASSERT(GC_mark_state == MS_INVALID);
  }
  GC_scan_ptr = NULL;
}

#ifdef PARALLEL_MARK
/* Initiate parallel marking. */
STATIC void GC_do_parallel_mark(void);

/*
 * The number of markers of the current mark phase, including the
 * collecting thread; one means that the collecting thread marks alone.
 * Set on every entry to `MS_ROOTS_PUSHED` state, with the allocator lock
 * held; read by the markers of a parallel mark phase (after acquiring
 * the mark lock to join the phase, thus after it is set).
 */
STATIC unsigned GC_phase_markers = 1;

#  define MAX_LOGGED_MARKER_DECISIONS 8

/*
 * The decisions of `markers_for_phase()` made since the previous call of
 * `GC_after_marking()`, to be logged by it (only the first
 * `MAX_LOGGED_MARKER_DECISIONS` of them are kept).  Protected by the
 * allocator lock.
 */
STATIC struct marker_decision {
  unsigned markers;
  unsigned max_markers;
  size_t bound;
  size_t live;
  size_t allocd;
  size_t roots;
  size_t stacks;
} GC_marker_decisions[MAX_LOGGED_MARKER_DECISIONS];

/*
 * The number of the decisions of `markers_for_phase()` made since the
 * previous call of `GC_after_marking()`, including those not kept.
 */
STATIC unsigned GC_n_marker_decisions = 0;

/*
 * Decide the number of markers of the mark phase whose roots have just
 * been pushed: one per `GC_bytes_per_marker` bytes the phase is estimated
 * to scan (rounded up), at least one and at most all the marker threads
 * plus the collecting thread; all of them if `GC_bytes_per_marker` is
 * zero.  An object reachable now was either live at the previous
 * collection or allocated since then, so the bytes the phase scans are
 * estimated by the size of the pointer-containing objects found live by
 * the previous collection, plus the bytes allocated since then, plus the
 * static data roots and the thread stacks pushed by this collection.
 * The estimate is an upper bound except for objects kept only by
 * conservative pointers to free or not yet swept objects, data pushed by
 * client push or mark procedures, and the live objects of the previous
 * collection that its mark counts missed (these counts can be low after
 * parallel marking).  Called with the allocator lock held, after each
 * push of the roots (thus again after a mark stack overflow).  This may
 * be with the world stopped, thus the decision is not logged here but
 * kept for `GC_after_marking()`.
 */
static unsigned
markers_for_phase(void)
{
  size_t per_marker = GC_bytes_per_marker;
  size_t live, allocd, roots, stacks, bound;
  unsigned markers = (unsigned)GC_markers_m1 + 1;
  unsigned res;

  GC_ASSERT(I_HOLD_LOCK());
  if (!GC_parallel || 0 == per_marker)
    return markers;
  live = (size_t)GC_composite_in_use;
  allocd = SIZET_SAT_ADD((size_t)GC_bytes_allocd,
                         (size_t)AO_load(&GC_bytes_allocd_tmp));
  roots = (size_t)GC_root_size;
  stacks = (size_t)GC_total_stacksize;
  bound = SIZET_SAT_ADD(SIZET_SAT_ADD(live, allocd),
                        SIZET_SAT_ADD(roots, stacks));
  if (bound / per_marker >= (size_t)markers) {
    res = markers;
  } else {
    /* Round up, and use one marker at least. */
    res = (unsigned)(bound / per_marker);
    if (bound % per_marker != 0 || 0 == res)
      res++;
  }
  if (UNLIKELY(GC_print_stats) && !GC_parallel_mark_disabled) {
    if (GC_n_marker_decisions < MAX_LOGGED_MARKER_DECISIONS) {
      struct marker_decision *d = &GC_marker_decisions[GC_n_marker_decisions];

      d->markers = res;
      d->max_markers = markers;
      d->bound = bound;
      d->live = live;
      d->allocd = allocd;
      d->roots = roots;
      d->stacks = stacks;
    }
    GC_n_marker_decisions++;
  }
  return res;
}

/* Log the decisions recorded by `markers_for_phase()`. */
static void
log_marker_decisions(void)
{
  unsigned i;
  unsigned n = GC_n_marker_decisions;

  GC_ASSERT(I_HOLD_LOCK());
  if (LIKELY(0 == n))
    return;
  GC_n_marker_decisions = 0;
  for (i = 0; i < n && i < MAX_LOGGED_MARKER_DECISIONS; i++) {
    const struct marker_decision *d = &GC_marker_decisions[i];

    GC_log_printf("Marking with %u of %u markers: about %lu KiB to scan"
                  " (live pointer data %lu, allocated %lu, roots %lu,"
                  " stacks %lu KiB)\n",
                  d->markers, d->max_markers, TO_KiB_UL(d->bound),
                  TO_KiB_UL(d->live), TO_KiB_UL(d->allocd),
                  TO_KiB_UL(d->roots), TO_KiB_UL(d->stacks));
  }
  if (n > MAX_LOGGED_MARKER_DECISIONS) {
    GC_log_printf("%u more decisions on the number of markers not logged\n",
                  n - MAX_LOGGED_MARKER_DECISIONS);
  }
}
#endif

#ifdef GC_DISABLE_INCREMENTAL
#  define GC_push_next_marked_dirty(h) GC_push_next_marked(h)
#else
STATIC struct hblk *GC_push_next_marked_dirty(struct hblk *h);
#endif /* !GC_DISABLE_INCREMENTAL */

STATIC struct hblk *GC_push_next_marked(struct hblk *h);
STATIC struct hblk *GC_push_next_marked_uncollectable(struct hblk *h);

static void alloc_mark_stack(size_t);
static size_t mark_stack_bytes(size_t);

/*
 * The events of the mark stacks since the previous call of
 * `GC_after_marking()`, which logs them.  They happen inside
 * `GC_mark_some()` or the marking of the finalizable objects, thus maybe
 * with the world stopped.  Protected by the allocator lock, except as
 * noted.
 */
STATIC struct {
  /* The overflows of the global mark stack which dropped entries. */
  word n_overflows;

  /* The size of the global mark stack at the last of these. */
  size_t overflow_size;

  /*
   * The overflows of the global mark stack which moved its entries to
   * the next mark stack, and the number of the moved entries.
   */
  word n_moves;
  word moved_entries;

  /*
   * The local mark stacks dropped because the global one had no room for
   * them.  Updated with the mark lock held (by any marker).
   */
  word n_no_room;

  /* The times the global mark stack grew. */
  word n_grown;

  /* The size of the global mark stack after it grew. */
  size_t grown_size;

  /* The size of the last failed growth of the global mark stack, or 0. */
  size_t failed_size;
} GC_mark_stack_events;

#ifdef PARALLEL_MARK
/*
 * The overflows of the local mark stacks since the previous call of
 * `GC_after_marking()`.  Updated by the markers without a lock.
 */
STATIC volatile AO_t GC_n_local_mark_stack_overflows = 0;
#endif

#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES) \
    && defined(GC_ASSERTIONS)
#  define CHECK_MARK_RING

/*
 * The number of the entry held in each slot of the global mark stack
 * during a parallel mark phase (see `GC_n_published`), written with the
 * slot, and checked by each marker that claims entries.  Allocated with
 * the global mark stack, after its last slot.
 */
STATIC volatile AO_t *GC_mark_ring_numbers = NULL;

/* The entries claimed by the markers in the current parallel mark phase. */
STATIC volatile AO_t GC_n_claimed = 0;

#  define MARK_STACK_ENTRY_BYTES (sizeof(mse) + sizeof(AO_t))
#else
#  define MARK_STACK_ENTRY_BYTES sizeof(mse)
#endif

/*
 * The head of the list of the outgrown mark stacks, to be recycled into
 * the heap by `GC_after_marking()`.  A mark stack is outgrown with the
 * world maybe stopped, and recycling it may log.  The link and the size
 * are stored at the start of the outgrown mark stack.  Protected by the
 * allocator lock.
 */
struct outgrown_mark_stack {
  struct outgrown_mark_stack *next;
  size_t bytes;
};

STATIC struct outgrown_mark_stack *GC_outgrown_mark_stacks = NULL;

/* Put a mark stack of `n` entries on the list of the outgrown ones. */
static void
add_outgrown_mark_stack(mse *stack, size_t n)
{
  struct outgrown_mark_stack *old = (struct outgrown_mark_stack *)stack;

  GC_ASSERT(I_HOLD_LOCK());
  old->next = GC_outgrown_mark_stacks;
  old->bytes = mark_stack_bytes(n);
  GC_outgrown_mark_stacks = old;
}

GC_INNER mse *GC_next_mark_stack = NULL;

/* The size of `GC_next_mark_stack`, in entries. */
STATIC size_t GC_next_mark_stack_size = 0;

/* The number of the entries moved to `GC_next_mark_stack`. */
STATIC size_t GC_n_moved_entries = 0;

static void
push_roots_and_advance(GC_bool push_all, ptr_t cold_gc_frame)
{
  if (GC_scan_ptr != NULL) {
    /* Not ready to push. */
    return;
  }
  /*
   * `GC_push_roots` marks some objects immediately (e.g. those referenced
   * from the registers, or from the static data regions if parallel
   * marking is on).  If a fault interrupts it, then `GC_mark_some`
   * invalidates the mark state, and the contents of these objects should
   * be pushed by the rescan from the marked objects, thus set the flag
   * beforehand.
   */
  GC_objects_are_marked = TRUE;
  GC_push_roots(push_all, cold_gc_frame);
  if (GC_mark_state != MS_INVALID) {
    GC_mark_state = MS_ROOTS_PUSHED;
#ifdef PARALLEL_MARK
    GC_phase_markers = markers_for_phase();
#endif
  }
}

STATIC GC_on_mark_stack_empty_proc GC_on_mark_stack_empty = 0;

GC_API void GC_CALL
GC_set_on_mark_stack_empty(GC_on_mark_stack_empty_proc fn)
{
  LOCK();
  GC_on_mark_stack_empty = fn;
  UNLOCK();
}

GC_API GC_on_mark_stack_empty_proc GC_CALL
GC_get_on_mark_stack_empty(void)
{
  GC_on_mark_stack_empty_proc fn;

  READER_LOCK();
  fn = GC_on_mark_stack_empty;
  READER_UNLOCK();
  return fn;
}

#ifdef WRAP_MARK_SOME
/*
 * This is called after we establish a structured exception or signal
 * handler, in case of a root segment is unmapped by the OS asynchronously.
 * Note that this code should never generate an incremental GC write fault.
 */
STATIC GC_bool
GC_mark_some_inner(ptr_t cold_gc_frame)
#else
GC_INNER GC_bool
GC_mark_some(ptr_t cold_gc_frame)
#endif
{
  GC_ASSERT(I_HOLD_LOCK());
  /* Every step ends with no entries in the next mark stack (see below). */
  GC_ASSERT(NULL == GC_next_mark_stack);
  switch (GC_mark_state) {
  case MS_NONE:
    return TRUE;

  case MS_PUSH_RESCUERS:
    if (ADDR_GE((ptr_t)GC_mark_stack_top,
                (ptr_t)(GC_mark_stack_limit - INITIAL_MARK_STACK_SIZE / 2))) {
      /*
       * Go ahead and mark.
       * TODO: This might cause us to see more marked dirty objects later on.
       */
      GC_mark_stack_too_small = TRUE;
      MARK_FROM_MARK_STACK();
    } else {
      GC_scan_ptr = GC_push_next_marked_dirty(GC_scan_ptr);
#ifndef GC_DISABLE_INCREMENTAL
      if (NULL == GC_scan_ptr) {
        GC_rescue_pass_ended = TRUE;
        GC_n_rescued_pages = GC_n_rescuing_pages;
      }
#endif
      push_roots_and_advance(FALSE, cold_gc_frame);
    }
    GC_ASSERT(GC_mark_state == MS_PUSH_RESCUERS
              || GC_mark_state == MS_ROOTS_PUSHED
              || GC_mark_state == MS_INVALID);
    break;

  case MS_PUSH_UNCOLLECTABLE:
    if (ADDR_GE((ptr_t)GC_mark_stack_top,
                (ptr_t)(GC_mark_stack + GC_mark_stack_size / 4))) {
#ifdef PARALLEL_MARK
      /* Avoid this, since we do not parallelize the marker here. */
      if (GC_parallel)
        GC_mark_stack_too_small = TRUE;
#endif
      MARK_FROM_MARK_STACK();
    } else {
      GC_scan_ptr = GC_push_next_marked_uncollectable(GC_scan_ptr);
      push_roots_and_advance(TRUE, cold_gc_frame);
    }
    GC_ASSERT(GC_mark_state == MS_PUSH_UNCOLLECTABLE
              || GC_mark_state == MS_ROOTS_PUSHED
              || GC_mark_state == MS_INVALID);
    break;

  case MS_ROOTS_PUSHED:
#ifdef PARALLEL_MARK
    /*
     * Eventually, incremental marking should run asynchronously in multiple
     * threads, without acquiring the allocator lock.
     * TODO: For now, parallel marker is disabled if there is a chance that
     * marking could be interrupted by a client-supplied time limit or custom
     * stop function.
     *
     * A mark phase whose estimated size gives it one marker only (see
     * `markers_for_phase()`) takes the serial path below as well.
     */
    if (GC_parallel && !GC_parallel_mark_disabled && GC_phase_markers > 1) {
      GC_do_parallel_mark();
      GC_mark_stack_top = GC_mark_stack - 1;
      if (GC_mark_stack_too_small) {
        alloc_mark_stack(2 * GC_mark_stack_size);
      }
      if (GC_mark_state == MS_ROOTS_PUSHED) {
        GC_mark_state = MS_NONE;
        return TRUE;
      }
      GC_ASSERT(GC_mark_state == MS_INVALID);
      break;
    }
#endif
    if (ADDR_GE((ptr_t)GC_mark_stack_top, (ptr_t)GC_mark_stack)) {
      MARK_FROM_MARK_STACK();
    } else {
      GC_on_mark_stack_empty_proc on_ms_empty = GC_on_mark_stack_empty;

      if (on_ms_empty != 0) {
        GC_mark_stack_top
            = on_ms_empty(GC_mark_stack_top, GC_mark_stack_limit);
        /* If we pushed new items, we need to continue processing. */
        if (ADDR_GE((ptr_t)GC_mark_stack_top, (ptr_t)GC_mark_stack))
          break;
      }
      if (GC_mark_stack_too_small) {
        alloc_mark_stack(2 * GC_mark_stack_size);
      }
      GC_mark_state = MS_NONE;
      return TRUE;
    }
    GC_ASSERT(GC_mark_state == MS_ROOTS_PUSHED || GC_mark_state == MS_INVALID);
    break;

  case MS_INVALID:
  case MS_PARTIALLY_INVALID:
    if (!GC_objects_are_marked) {
      GC_mark_state = MS_PUSH_UNCOLLECTABLE;
      break;
    }
    if (ADDR_GE((ptr_t)GC_mark_stack_top, (ptr_t)GC_mark_stack)) {
      MARK_FROM_MARK_STACK();
      GC_ASSERT(GC_mark_state == MS_PARTIALLY_INVALID
                || GC_mark_state == MS_INVALID);
      break;
    }
    if (NULL == GC_scan_ptr && GC_mark_state == MS_INVALID) {
      /*
       * About to start a heap scan for marked objects.
       * Mark stack is empty.  OK to reallocate.
       */
      if (GC_mark_stack_too_small) {
        alloc_mark_stack(2 * GC_mark_stack_size);
      }
      GC_mark_state = MS_PARTIALLY_INVALID;
    }
    GC_scan_ptr = GC_push_next_marked(GC_scan_ptr);
    if (GC_mark_state == MS_PARTIALLY_INVALID)
      push_roots_and_advance(TRUE, cold_gc_frame);
    GC_ASSERT(GC_mark_state == MS_ROOTS_PUSHED
              || GC_mark_state == MS_PARTIALLY_INVALID
              || GC_mark_state == MS_INVALID);
    break;

  default:
    ABORT("GC_mark_some: bad state");
  }
  if (UNLIKELY(GC_next_mark_stack != NULL)) {
    /*
     * The global mark stack overflowed in a push outside `GC_mark_from()`
     * (of the roots, of the marked objects of a block, or by the procedure
     * set by `GC_set_on_mark_stack_empty()`).
     */
    GC_switch_to_next_mark_stack();
  }
  return FALSE;
}

#ifdef PARALLEL_MARK
GC_INNER GC_bool GC_parallel_mark_disabled = FALSE;

#  ifndef GC_BYTES_PER_MARKER
#    define GC_BYTES_PER_MARKER 0
#  endif

GC_INNER size_t GC_bytes_per_marker = (size_t)GC_BYTES_PER_MARKER;
#endif

GC_API void GC_CALL
GC_set_bytes_per_marker(size_t value)
{
#ifdef PARALLEL_MARK
  LOCK();
  GC_bytes_per_marker = value;
  UNLOCK();
#else
  UNUSED_ARG(value);
#endif
}

GC_API size_t GC_CALL
GC_get_bytes_per_marker(void)
{
#ifdef PARALLEL_MARK
  size_t value;

  READER_LOCK();
  value = GC_bytes_per_marker;
  READER_UNLOCK();
  return value;
#else
  return 0;
#endif
}

#ifdef WRAP_MARK_SOME
GC_INNER GC_bool
GC_mark_some(ptr_t cold_gc_frame)
{
  GC_bool ret_val;

  if (GC_no_dls) {
    ret_val = GC_mark_some_inner(cold_gc_frame);
  } else {
    /*
     * Windows appears to asynchronously create and remove writable memory
     * mappings, in particular in case of `DllMain`-based thread tracking.
     * Since we look for writable regions to determine the root set, we may
     * try to mark from an address range that disappeared since we started
     * the collection.  Thus we have to recover from faults here.
     * This code also seems to be necessary for WinCE (at least in the case
     * we would decide to add `MEM_PRIVATE` sections to data roots in
     * `GC_register_dynamic_libraries`).
     *
     * If `USE_PROC_FOR_LIBRARIES` is defined, then we are handling the case
     * in which `/proc` is used for root finding, and we have threads.
     * We may find a stack for a thread that is in the process of exiting,
     * and disappears while we are marking it.  This seems extremely
     * difficult to avoid otherwise.
     */
#  ifndef NO_SEH_AVAILABLE
#    if GC_CLANG_PREREQ(3, 6)
#      pragma GCC diagnostic push
#      pragma GCC diagnostic ignored "-Wlanguage-extension-token"
#    endif
    __try {
      ret_val = GC_mark_some_inner(cold_gc_frame);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      goto handle_ex;
    }
#    if GC_CLANG_PREREQ(3, 6)
#      pragma GCC diagnostic pop
#    endif
#  else
    GC_setup_temporary_fault_handler();
    if (SETJMP(GC_jmp_buf) != 0)
      goto handle_ex;
    ret_val = GC_mark_some_inner(cold_gc_frame);
    GC_reset_fault_handler();
#  endif
  }

#  ifdef HAS_WIN32_THREADS_DISCOVERY
  /*
   * With `DllMain`-based thread tracking, a thread may have started
   * while we were marking.  This is logically equivalent to the
   * exception case; our results are invalid and we have to start over.
   * This cannot be prevented since we cannot block in `DllMain()`.
   */
  if (GC_started_thread_while_stopped())
    goto handle_thr_start;
#  endif
  return ret_val;

handle_ex:
  /* Exception handler starts here for all cases. */
#  if defined(NO_SEH_AVAILABLE)
  GC_reset_fault_handler();
#  endif
  {
    static word warned_gc_no;

    /* Report caught `ACCESS_VIOLATION`, once per collection. */
    if (warned_gc_no != GC_gc_no) {
      GC_COND_LOG_PRINTF("Memory mapping disappeared at collection #%lu\n",
                         (unsigned long)GC_gc_no + 1);
      warned_gc_no = GC_gc_no;
    }
  }
#  ifdef HAS_WIN32_THREADS_DISCOVERY
handle_thr_start:
#  endif
  /*
   * We have bad roots on the mark stack - discard it.
   * Rescan from the marked objects; redetermine the roots.
   */
#  ifdef USE_PROC_FOR_LIBRARIES
  {
    size_t idx_p1 = GC_push_root_idx_p1;

    if (0 == idx_p1)
      ABORT("SIGSEGV occurred outside GC_push_conditional_with_exclusions");
    GC_remove_root_at_pos(idx_p1 - 1);
    GC_push_root_idx_p1 = 0;
  }
#  endif
  GC_invalidate_mark_state();
  GC_scan_ptr = NULL;
  return FALSE;
}
#endif /* WRAP_MARK_SOME */

GC_INNER void
GC_invalidate_mark_state(void)
{
  GC_mark_state = MS_INVALID;
  GC_mark_stack_top = GC_mark_stack - 1;
  if (UNLIKELY(GC_next_mark_stack != NULL)) {
    /*
     * A fault interrupted a step of `GC_mark_some()` while entries waited
     * in the next mark stack.  Drop these as well, and use the larger
     * mark stack.
     */
    GC_n_moved_entries = 0;
    GC_switch_to_next_mark_stack();
    GC_ASSERT(GC_mark_stack_empty());
  }
}

#ifndef GC_NO_FINALIZATION
GC_INNER void
GC_rescan_marked_objects(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_mark_stack_empty());
  GC_ASSERT(GC_mark_state == MS_INVALID);
  do {
    struct hblk *h;

    if (GC_mark_stack_too_small)
      alloc_mark_stack(2 * GC_mark_stack_size);
    GC_mark_state = MS_PARTIALLY_INVALID;
    /* Start from the first heap block in use. */
    h = GC_next_block(NULL, FALSE);
    while (h != NULL) {
      h = GC_push_next_marked(h);
      if (UNLIKELY(GC_next_mark_stack != NULL)) {
        /* The push of the marked objects of the block overflowed. */
        GC_switch_to_next_mark_stack();
      }
      while (!GC_mark_stack_empty())
        MARK_FROM_MARK_STACK();
    }
    /*
     * An overflow during the scan which drops entries sets the state
     * to `MS_INVALID`.
     */
  } while (GC_mark_state == MS_INVALID);
  GC_ASSERT(NULL == GC_next_mark_stack);
  GC_mark_state = MS_NONE;
}
#endif /* !GC_NO_FINALIZATION */

/*
 * Request a larger global mark stack after an overflow of it dropped
 * entries, and record the overflow for `GC_after_marking()`.  Only the
 * thread holding the allocator lock marks on the global mark stack.
 */
GC_INLINE void
global_mark_stack_overflow(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_mark_stack_too_small = TRUE;
  GC_mark_stack_events.n_overflows++;
  GC_mark_stack_events.overflow_size = GC_mark_stack_size;
}

/*
 * Move all the entries of the global mark stack, which is full (`msp` is
 * its limit), to the next mark stack, after the entries moved there
 * before.  The next mark stack is allocated, or replaced by a larger one,
 * unless it has room for these entries and then for a full global mark
 * stack more.  Return `FALSE` if no such mark stack could be allocated;
 * nothing is moved then.
 */
static GC_bool
move_to_next_mark_stack(const mse *msp)
{
  size_t n = (size_t)(msp - GC_mark_stack);

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(msp == GC_mark_stack_limit && n == GC_mark_stack_size);
#ifdef PARALLEL_MARK
  /* No parallel mark phase is running. */
  GC_ASSERT(0 == AO_load(&GC_helper_count));
#endif
  if (NULL == GC_next_mark_stack
      || GC_next_mark_stack_size - GC_n_moved_entries < 2 * n) {
    const size_t max_size = GC_SIZE_MAX / MARK_STACK_ENTRY_BYTES;
    size_t needed, new_size;
    mse *new_stack;

    if (UNLIKELY(n > (max_size - GC_n_moved_entries) / 2)) {
      GC_mark_stack_events.failed_size = max_size;
      return FALSE;
    }
    needed = GC_n_moved_entries + 2 * n;
    /*
     * Use for entries the `MINHINCR * HBLKSIZE` bytes which a mark stack
     * gets at least (see `mark_stack_bytes()`).  Keep the size a power
     * of two.
     */
    if (needed < (size_t)MINHINCR * HBLKSIZE / MARK_STACK_ENTRY_BYTES)
      needed = (size_t)MINHINCR * HBLKSIZE / MARK_STACK_ENTRY_BYTES;
    for (new_size = 1; new_size < needed; new_size <<= 1) {
      if (UNLIKELY(new_size > max_size / 2)) {
        GC_mark_stack_events.failed_size = max_size;
        return FALSE;
      }
    }
    new_stack = (mse *)GC_scratch_alloc(mark_stack_bytes(new_size));
    if (UNLIKELY(NULL == new_stack)) {
      GC_mark_stack_events.failed_size = new_size;
      return FALSE;
    }
    if (GC_next_mark_stack != NULL) {
      BCOPY(GC_next_mark_stack, new_stack, GC_n_moved_entries * sizeof(mse));
      /* It was allocated in this collection, thus with the right flags. */
      add_outgrown_mark_stack(GC_next_mark_stack, GC_next_mark_stack_size);
    }
    GC_next_mark_stack = new_stack;
    GC_next_mark_stack_size = new_size;
  }
  BCOPY(GC_mark_stack, GC_next_mark_stack + GC_n_moved_entries,
        n * sizeof(mse));
  GC_n_moved_entries += n;
  GC_mark_stack_events.n_moves++;
  GC_mark_stack_events.moved_entries += n;
  return TRUE;
}

/*
 * Handle an overflow of a mark stack, i.e. a push of an entry at `msp`,
 * which is `mark_stack_limit`.  Return the address to write the entry at.
 *
 * The global mark stack overflows in serial marking, which the thread
 * holding the allocator lock does.  Then all its entries are moved to
 * the next mark stack, and the entry is written at its bottom.  The
 * global mark stack itself (its address, size and limit) does not change,
 * since the callers keep its bounds in local variables: `GC_mark_from()`,
 * `GC_push_marked()` and the client mark procedures, for example.  The
 * next mark stack has room for a full global mark stack more, thus
 * `GC_switch_to_next_mark_stack()` makes it the global mark stack
 * without allocating, where no caller keeps the bounds: after each
 * `GC_mark_from()` call on the global mark stack (in
 * `MARK_FROM_MARK_STACK()`), at the end of each step of `GC_mark_some()`,
 * after the pushes outside `GC_mark_some()` (by the mark procedure of
 * a finalizable object in `GC_mark_fo()`, and of the marked objects of
 * a block in `GC_rescan_marked_objects()`), and in
 * `GC_invalidate_mark_state()`.  The memory of the next mark stack is
 * thus obtained from the OS during the marking, maybe with the world
 * stopped and inside a client mark procedure, and the procedure set by
 * `GC_set_on_os_get_mem()` is called there.
 *
 * If that memory cannot be obtained, or a marker of a parallel mark phase
 * overflows its local mark stack, then the top `GC_MARK_STACK_DISCARDS`
 * entries are dropped, and the mark state is invalidated.  The objects of
 * the dropped entries are marked, thus `GC_mark_some()` completes the
 * marking by marking from all the marked objects again.
 *
 * This is not inlined: it is called rarely, and inlined into each push of
 * `GC_mark_from()` it makes the marking loop slower.
 */
GC_ATTR_NOINLINE
STATIC mse *
GC_signal_mark_stack_overflow(mse *msp, const mse *mark_stack_limit)
{
  size_t discards = GC_MARK_STACK_DISCARDS;

#ifdef PARALLEL_MARK
  /*
   * If a marker overflows its `local_mark_stack` in the parallel mode,
   * then do not signal the global mark stack to be resized.  That will
   * be done in `GC_return_mark_stack` if required.  But the global mark
   * stack is still used by the serial marking even if `GC_parallel`,
   * e.g. if the parallel marker is disabled because of a stop function or
   * a time limit, or during the mark state recovery after an overflow.
   */
  if (GC_parallel && mark_stack_limit != GC_mark_stack_limit) {
    (void)AO_fetch_and_add1(&GC_n_local_mark_stack_overflows);
    if (discards > LOCAL_MARK_STACK_SIZE / 8) {
      /*
       * `GC_MARK_STACK_DISCARDS` depends on `INITIAL_MARK_STACK_SIZE`
       * (which could be defined by the client), thus it might exceed
       * the capacity of a local mark stack.
       */
      discards = LOCAL_MARK_STACK_SIZE / 8;
    }
  } else
#else
  UNUSED_ARG(mark_stack_limit);
#endif
  /* else */ {
    if (LIKELY(move_to_next_mark_stack(msp)))
      return GC_mark_stack;
    global_mark_stack_overflow();
  }
  GC_mark_state = MS_INVALID;
#ifdef GC_ASSERTIONS
  {
    /* The overflowing mark stack is the global one, or a local one. */
    const mse *mark_stack = GC_mark_stack;

#  ifdef PARALLEL_MARK
    if (mark_stack_limit != GC_mark_stack_limit)
      mark_stack = mark_stack_limit - LOCAL_MARK_STACK_SIZE;
#  endif
    /* The new top should remain inside the mark stack. */
    GC_ASSERT((word)(msp - mark_stack) >= discards);
  }
#endif
#if defined(CPPCHECK)
  GC_noop1_ptr(msp);
#endif
  return msp - discards;
}

GC_ATTR_NO_SANITIZE_ADDR_MEM_THREAD
GC_INNER mse *
GC_mark_from(mse *mark_stack_top, const mse *mark_stack, mse *mark_stack_limit)
{
  GC_signed_word credit = HBLKSIZE; /*< remaining credit for marking work */
  word descr;
  ptr_t current_p;    /*< pointer to the current candidate pointer */
  ptr_t q;            /*< the candidate pointer itself */
  ptr_t limit = NULL; /*< the limit (incl.) of the current candidate range */
  ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
  DECLARE_HDR_CACHE;

#define SPLIT_RANGE_PTRS 128 /*< must be power of 2 */

  /*
   * A call on the global mark stack is made by `MARK_FROM_MARK_STACK()`,
   * which switches to the next mark stack after each such call.
   */
  GC_ASSERT(mark_stack != GC_mark_stack || NULL == GC_next_mark_stack);
  /*
   * Every marker calls this function often.  A store, even of the unchanged
   * value, would take the cache line holding the flag away from the other
   * markers.
   */
  if (!GC_objects_are_marked)
    GC_objects_are_marked = TRUE;
  INIT_HDR_CACHE;
#if defined(OS2) || CPP_PTRSZ > CPP_WORDSZ
  /* OS/2: avoid the tweaked variant to circumvent a compiler problem. */
  while (ADDR_GE((ptr_t)mark_stack_top, (ptr_t)mark_stack) && credit >= 0)
#else
  while (((((word)mark_stack_top - (word)mark_stack) | (word)credit) & SIGNB)
         == 0)
#endif
  {
    current_p = mark_stack_top->mse_start;
    descr = mark_stack_top->mse_descr;
  retry:
    /*
     * `current_p` and `descr` describe the current object.
     * `*mark_stack_top` is vacant.
     * The following is zero only for small objects described by a simple
     * length descriptor.  For many applications this is the common case,
     * so we try to detect it quickly.
     */
    if (descr & (~(word)(PTRS_TO_BYTES(SPLIT_RANGE_PTRS) - 1) | GC_DS_TAGS)) {
      word tag = descr & GC_DS_TAGS;

      GC_STATIC_ASSERT(GC_DS_TAGS == 0x3);
      switch (tag) {
      case GC_DS_LENGTH:
        /*
         * Large length.  Process part of the range to avoid pushing
         * too much on the stack.
         */

        /* Either it is a heap object or a region outside the heap. */
        GC_ASSERT(descr < GC_greatest_real_heap_addr - GC_least_real_heap_addr
                  || GC_least_real_heap_addr + sizeof(ptr_t)
                         >= ADDR(current_p) + descr
                  || ADDR(current_p) >= GC_greatest_real_heap_addr);
#ifdef PARALLEL_MARK
#  define SHARE_BYTES 2048
        if (descr > SHARE_BYTES && GC_parallel
            && ADDR_LT((ptr_t)mark_stack_top, (ptr_t)(mark_stack_limit - 1))) {
          word new_size = (descr >> 1) & ~(word)(sizeof(ptr_t) - 1);

          mark_stack_top->mse_start = current_p;
          /* This makes sure we handle misaligned pointers. */
          mark_stack_top->mse_descr
              = (new_size + sizeof(ptr_t)) | GC_DS_LENGTH;
          mark_stack_top++;
#  ifdef ENABLE_TRACE
          if (ADDR_INSIDE(GC_trace_ptr, current_p, current_p + descr)) {
            GC_log_printf("GC #%lu: large section; start %p, len %lu,"
                          " splitting (parallel) at %p\n",
                          (unsigned long)GC_gc_no, (void *)current_p,
                          (unsigned long)descr,
                          (void *)(current_p + new_size));
          }
#  endif
          current_p += new_size;
          descr -= new_size;
          goto retry;
        }
#endif /* PARALLEL_MARK */
        limit = current_p + PTRS_TO_BYTES(SPLIT_RANGE_PTRS - 1);
        mark_stack_top->mse_start = limit;
        mark_stack_top->mse_descr
            = descr - PTRS_TO_BYTES(SPLIT_RANGE_PTRS - 1);
#ifdef ENABLE_TRACE
        if (ADDR_INSIDE(GC_trace_ptr, current_p, current_p + descr)) {
          GC_log_printf(
              "GC #%lu: large section; start %p, len %lu, splitting at %p\n",
              (unsigned long)GC_gc_no, (void *)current_p, (unsigned long)descr,
              (void *)limit);
        }
#endif
        /*
         * Make sure that pointers overlapping the two ranges are
         * considered.
         */
        limit += sizeof(ptr_t) - ALIGNMENT;
        break;
      case GC_DS_BITMAP:
        mark_stack_top--;
#ifdef ENABLE_TRACE
        if (ADDR_INSIDE(GC_trace_ptr, current_p,
                        current_p + PTRS_TO_BYTES(BITMAP_BITS))) {
          GC_log_printf("GC #%lu: tracing from %p bitmap descr 0x%lx\n",
                        (unsigned long)GC_gc_no, (void *)current_p,
                        (unsigned long)descr);
        }
#endif
        descr &= ~(word)GC_DS_TAGS;
        credit -= (GC_signed_word)PTRS_TO_BYTES(CPP_PTRSZ / 2); /*< guess */
        for (; descr != 0;
             descr <<= 1, current_p += sizeof(ptr_t)) { /*< not `ALIGNMENT` */
          if ((descr & SIGNB) == 0)
            continue;
          LOAD_PTR_OR_CONTINUE(q, current_p);
          FIXUP_POINTER(q);
          if (ADDR_LT(least_ha, q) && ADDR_LT(q, greatest_ha)) {
            PREFETCH_CANDIDATE(q);
#ifdef ENABLE_TRACE
            if (GC_trace_ptr == current_p) {
              GC_log_printf("GC #%lu: considering(3) %p -> %p\n",
                            (unsigned long)GC_gc_no, (void *)current_p,
                            (void *)q);
            }
#endif
            PUSH_CONTENTS(q, mark_stack_top, mark_stack_limit, current_p);
          }
        }
        continue;
      case GC_DS_PROC:
        mark_stack_top--;
#ifdef ENABLE_TRACE
        if (ADDR_GE(GC_trace_ptr, current_p)) {
          const void *base = GC_base(current_p);

          if (base != NULL && GC_base(GC_trace_ptr) == base) {
            GC_log_printf("GC #%lu: tracing from %p, proc descr 0x%lx\n",
                          (unsigned long)GC_gc_no, (void *)current_p,
                          (unsigned long)descr);
          }
        }
#endif
        credit -= GC_PROC_BYTES;
        mark_stack_top = (*PROC(descr))((word *)current_p, mark_stack_top,
                                        mark_stack_limit, ENV(descr));
        continue;
      case GC_DS_PER_OBJECT:
        if (!(descr & SIGNB)) {
          /* Descriptor is in the object. */
          descr = *(word *)(current_p + descr - GC_DS_PER_OBJECT);
        } else {
          /*
           * Descriptor is in the type descriptor pointed to by the first
           * "pointer-sized" word of the object.
           */
          ptr_t type_descr = *(ptr_t *)current_p;

          /*
           * `type_descr` is either a valid pointer to the descriptor
           * structure, or this object was on a free list.
           * If it was anything but the last object on the free list,
           * we will misinterpret the next object on the free list as
           * the type descriptor, and get a zero GC descriptor, which
           * is ideal.  Unfortunately, we need to check for the last
           * object case explicitly.
           */
          if (UNLIKELY(NULL == type_descr)) {
            mark_stack_top--;
            continue;
          }
          if (IS_INDIR_PER_OBJ_DESCR(descr)) {
            /* Use the bitmap to check if the object is on a free list. */
            hdr *hhdr;
            size_t bit_no = GC_get_hhdr_mark_bit_no(&hhdr, current_p);
            const valid_ds_bitmap_t *bitmap = hhdr->hb_valid_ds_bitmap;

            if (bitmap != NULL
                && UNLIKELY(!hdr_valid_ds_mark(bitmap, bit_no))) {
              mark_stack_top--;
              continue;
            }
          }
          descr = *(word *)(type_descr
                            - ((GC_signed_word)descr
                               + (GC_INDIR_PER_OBJ_BIAS - GC_DS_PER_OBJECT)));
        }
        if (0 == descr) {
          /*
           * Can happen either because we generated a zero GC descriptor
           * or we saw a pointer to a free object.
           */
          mark_stack_top--;
          continue;
        }
        goto retry;
      }
    } else {
      /* Small object with length descriptor. */
      mark_stack_top--;
#ifndef SMALL_CONFIG
      if (descr < sizeof(ptr_t))
        continue;
#endif
#ifdef ENABLE_TRACE
      if (ADDR_INSIDE(GC_trace_ptr, current_p, current_p + descr)) {
        GC_log_printf("GC #%lu: small object; start %p, len %lu\n",
                      (unsigned long)GC_gc_no, (void *)current_p,
                      (unsigned long)descr);
      }
#endif
      limit = current_p + descr;
    }
    /* The simple case in which we are scanning a range. */
    GC_ASSERT((ADDR(current_p) & (ALIGNMENT - 1)) == 0);
    credit -= limit - current_p;
    limit -= sizeof(ptr_t);
    {
#define PREF_DIST 4

#if !defined(SMALL_CONFIG) && !(defined(E2K) && defined(USE_PTR_HWTAG))
      ptr_t deferred;

#  ifdef CHERI_PURECAP
      /*
       * Check each pointer for validity before dereferencing to prevent
       * capability exceptions.  Utilize the pointer meta-data to speed-up
       * the loop.  If the loop is below the pointer bounds, skip the rest
       * of marking for that chunk.  If the limit capability restricts us to
       * reading fewer than size of a pointer, then there cannot possibly be
       * a pointer at `limit`'s pointer, and reading at that location will
       * raise a capability exception.
       */
      {
        word cap_limit = cheri_base_get(limit) + cheri_length_get(limit);

        if (ADDR(limit) + sizeof(ptr_t) > cap_limit) {
          /* Decrement limit so that it to be within bounds of `current_p`. */
          GC_ASSERT(cap_limit > sizeof(ptr_t));
          limit = (ptr_t)cheri_address_set(
              current_p, (cap_limit - sizeof(ptr_t)) & ~(sizeof(ptr_t) - 1));
          goto check_limit;
        }
      }
#  endif
      /*
       * Try to prefetch the next pointer to be examined as soon as possible.
       * Empirically, this also seems to help slightly without prefetches,
       * at least on Linux/i686.  Presumably this loop ends up with less
       * register pressure, and gcc thus ends up generating slightly better
       * code.  Overall gcc code quality for this loop is still not great.
       */
      for (;;) {
        PREFETCH(limit - PREF_DIST * CACHE_LINE_SIZE);
        GC_ASSERT(ADDR_GE(limit, current_p));
#  ifdef CHERI_PURECAP
        if (ADDR(limit) < cheri_base_get(limit))
          goto next_object;
        if (!HAS_TAG_AND_PERM_LOAD(limit)) {
          limit -= ALIGNMENT;
          goto check_limit;
        }
#  endif
        deferred = *(ptr_t *)limit;
        FIXUP_POINTER(deferred);
        limit -= ALIGNMENT;
#  ifdef CHERI_PURECAP
        if (!HAS_TAG_AND_PERM_LOAD(deferred))
          goto check_limit;
#  endif
        if (ADDR_LT(least_ha, deferred) && ADDR_LT(deferred, greatest_ha)) {
          /*
           * Prefetch this candidate as soon as it is found, before it is
           * known to be unmarked and not pointer-free.  If it is pushed,
           * it is pushed last and popped next, so the scan of the rest
           * of the range below is all the time there is for the fetch.
           * This wastes at most one prefetch per range.
           */
          PREFETCH(deferred);
          break;
        }
#  ifndef CHERI_PURECAP
        if (ADDR_LT(limit, current_p))
          goto next_object;
        /*
         * Unroll once, so we do not do too many of the prefetches based
         * on `limit`.
         */
        deferred = *(ptr_t *)limit;
        FIXUP_POINTER(deferred);
        limit -= ALIGNMENT;
        if (ADDR_LT(least_ha, deferred) && ADDR_LT(deferred, greatest_ha)) {
          PREFETCH(deferred);
          break;
        }
#  else
      check_limit:
#  endif
        if (ADDR_LT(limit, current_p))
          goto next_object;
      }
#endif

      for (; ADDR_GE(limit, current_p); current_p += ALIGNMENT) {
        /*
         * Empirically, unrolling this loop does not help a lot.
         * Since `PUSH_CONTENTS` expands to a lot of code, we do not.
         */
        LOAD_PTR_OR_CONTINUE(q, current_p);
        FIXUP_POINTER(q);
        PREFETCH(current_p + PREF_DIST * CACHE_LINE_SIZE);
        if (ADDR_LT(least_ha, q) && ADDR_LT(q, greatest_ha)) {
          PREFETCH_CANDIDATE(q);
#ifdef ENABLE_TRACE
          if (GC_trace_ptr == current_p) {
            GC_log_printf("GC #%lu: considering(1) %p -> %p\n",
                          (unsigned long)GC_gc_no, (void *)current_p,
                          (void *)q);
          }
#endif
          PUSH_CONTENTS(q, mark_stack_top, mark_stack_limit, current_p);
        }
      }

#if !defined(SMALL_CONFIG) && !(defined(E2K) && defined(USE_PTR_HWTAG))
      /*
       * We still need to mark the entry we previously prefetched.
       * We already know that it passes the preliminary pointer validity test.
       */
#  ifdef ENABLE_TRACE
      if (GC_trace_ptr == current_p) {
        GC_log_printf("GC #%lu: considering(2) %p -> %p\n",
                      (unsigned long)GC_gc_no, (void *)current_p,
                      (void *)deferred);
      }
#  endif
      PUSH_CONTENTS(deferred, mark_stack_top, mark_stack_limit, current_p);
    next_object:;
#endif
    }
  }
  return mark_stack_top;
}

#ifdef PARALLEL_MARK

/* Note: this is protected by the mark lock. */
STATIC GC_bool GC_help_wanted = FALSE;

#  ifdef ATOMIC_FL_BUILDER_COUNT
GC_INNER void
GC_fl_builder_enter(void)
{
  /*
   * Only a holder of the allocator lock may increment the count, and
   * a waiter for the builders holds the allocator lock while waiting.
   * Thus the count cannot increase while somebody waits for it to reach
   * zero, and no memory barrier is needed here: `UNLOCK()` publishes the
   * new value to the next holder of the allocator lock.
   */
  GC_ASSERT(I_HOLD_LOCK());
  (void)AO_fetch_and_add(&GC_fl_builder_count, FL_BUILDER_ONE);
}

GC_INNER void
GC_fl_builder_leave(void)
{
  GC_ASSERT(I_DONT_HOLD_LOCK());
  for (;;) {
    AO_t v = AO_load(&GC_fl_builder_count);

    if (v == (FL_BUILDER_ONE | FL_BUILDER_WAITING))
      break;
    /*
     * Either there are other builders, or nobody waits for us.  The
     * release barrier makes the built free list (and our reads of the
     * mark bits) complete before the collector could observe the
     * decremented count.
     */
    if (AO_compare_and_swap_release(&GC_fl_builder_count, v,
                                    v - FL_BUILDER_ONE))
      return;
  }

  /*
   * We were the last builder when the collector waited for us.  Holding
   * the mark lock, decrement the count, and if only the flag is left,
   * clear the flag and notify the waiter, as the marker threads startup
   * does.  Both are read-modify-writes, so that they continue the
   * release sequences of the decrements by the other builders, and the
   * acquire load of zero by the waiter synchronizes with all of them.
   */
  GC_acquire_mark_lock();
  if (AO_fetch_and_add(&GC_fl_builder_count, (AO_t)0 - FL_BUILDER_ONE)
          - FL_BUILDER_ONE
      == FL_BUILDER_WAITING) {
    (void)AO_fetch_and_add(&GC_fl_builder_count, (AO_t)0 - FL_BUILDER_WAITING);
    GC_notify_all_builder();
  }
  GC_release_mark_lock();
}

GC_INNER GC_bool
GC_fl_builders_to_wait(void)
{
  /* Holding the allocator lock prevents new builders from entering. */
  GC_ASSERT(I_HOLD_LOCK());
  for (;;) {
    AO_t v = AO_load_acquire(&GC_fl_builder_count);

    /* The marker threads startup can make the count negative. */
    if ((GC_signed_word)v <= 0)
      return FALSE;
    if ((v & FL_BUILDER_WAITING) != 0
        || AO_compare_and_swap_release(&GC_fl_builder_count, v,
                                       v | FL_BUILDER_WAITING))
      return TRUE;
  }
}
#  else
GC_INNER void
GC_fl_builder_enter(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_acquire_mark_lock();
  ++GC_fl_builder_count;
  GC_release_mark_lock();
}

GC_INNER void
GC_fl_builder_leave(void)
{
  GC_ASSERT(I_DONT_HOLD_LOCK());
  GC_acquire_mark_lock();
  if (0 == --GC_fl_builder_count)
    GC_notify_all_builder();
  GC_release_mark_lock();
}
#  endif /* !ATOMIC_FL_BUILDER_COUNT */

GC_INNER void
GC_wait_for_markers_init(void)
{
  GC_signed_word count;

  GC_ASSERT(I_HOLD_LOCK());
  if (0 == GC_markers_m1)
    return;

#  ifndef CAN_HANDLE_FORK
  GC_ASSERT(NULL == GC_main_local_mark_stack);
#  else
  if (NULL == GC_main_local_mark_stack)
#  endif
  {
    size_t bytes_to_get
        = ROUNDUP_PAGESIZE_IF_MMAP(LOCAL_MARK_STACK_SIZE * sizeof(mse));

    /*
     * Allocate the local mark stack for the thread that holds the
     * allocator lock.
     */
    GC_ASSERT(GC_page_size != 0);
    GC_main_local_mark_stack = (mse *)GC_os_get_mem(bytes_to_get);
    if (NULL == GC_main_local_mark_stack)
      ABORT("Insufficient memory for main local_mark_stack");
  }

  /*
   * Reuse the mark lock and builders count to synchronize marker threads
   * startup.
   */
  GC_acquire_mark_lock();
#  ifdef ATOMIC_FL_BUILDER_COUNT
  /* Note: `count` is twice the number here. */
  count = (GC_signed_word)AO_fetch_and_add(
              &GC_fl_builder_count, (AO_t)GC_markers_m1 * FL_BUILDER_ONE)
          + GC_markers_m1 * (GC_signed_word)FL_BUILDER_ONE;
#  else
  GC_fl_builder_count += GC_markers_m1;
  count = GC_fl_builder_count;
#  endif
  GC_release_mark_lock();
  if (count != 0) {
    GC_ASSERT(count > 0);
    GC_wait_for_reclaim();
  }
}

#  ifdef STEAL_MARK_STACK_RANGES
/*
 * The index of the slot of the global mark stack that holds the entry
 * numbered `i` during a parallel mark phase.
 */
#    define MARK_RING_INDEX(i) ((size_t)((i) & (AO_t)(GC_mark_stack_size - 1)))

/*
 * Copy `n` entries from `from` to `to`.  One of the two is a range of
 * slots of the global mark stack.  A marker whose claim fails may read
 * slots while a marker copying back its local mark stack writes them (see
 * `GC_steal_mark_stack_range`), and discards what it read.  The race is
 * benign, but the thread sanitizer would report it, thus each word is
 * read and written atomically if the sanitizer is on.
 */
GC_INLINE void
copy_mark_entries(mse *to, mse *from, size_t n)
{
#    ifdef THREAD_SANITIZER
  size_t i;

  for (i = 0; i < n; i++) {
    GC_cptr_store(&to[i].mse_start, GC_cptr_load(&from[i].mse_start));
    AO_store(&to[i].mse_descr, AO_load(&from[i].mse_descr));
  }
#    else
  BCOPY(from, to, n * sizeof(mse));
#    endif
}

/*
 * Try to claim the entries of the global mark stack numbered from `first`
 * (the value of `GC_first_unclaimed` observed by the caller) up to `end`
 * (exclusive; the value of `GC_n_published` observed by the caller with an
 * acquire load or holding the mark lock), or fewer if we have `n_to_get`
 * entries, counting a big object as in `GC_steal_mark_stack`.  The entries
 * are counted by their descriptors, copied into mark stack `local`, and
 * then claimed by a CAS of `GC_first_unclaimed` from `first` with release
 * ordering.  Return the number of the claimed entries, or zero if another
 * marker has claimed entries since `first` was observed: then either the
 * entries are not copied (if `GC_first_unclaimed` has moved by the end of
 * the count), or the CAS fails and the copies are discarded.
 *
 * The CAS claims the copied entries exclusively, and they are the right
 * ones.  `GC_first_unclaimed` never decreases during the phase, thus it
 * equaled `first` from the load of the caller to a successful CAS, i.e.
 * no entry from `first` on was claimed in the meantime.  A marker writes
 * the slot of entry `j` (in `GC_return_mark_stack`) only after it has
 * observed, with an acquire load, that `GC_first_unclaimed` exceeds
 * `j - GC_mark_stack_size`, i.e. that the entry held in the slot before
 * has been claimed.  Thus none of the slots counted and copied here was
 * written between the release store of `GC_n_published` that published
 * `end` and the CAS.  After a successful CAS, the release ordering makes
 * the reads of the slots happen before any later write of them.  This
 * relies on every write of `GC_first_unclaimed` during the phase being
 * a CAS: the CAS of a later claim continues the release sequence of an
 * earlier one, so a marker that observes the later claim also synchronizes
 * with the earlier one.  A claim whose CAS fails may have read a slot that
 * was being written (see `copy_mark_entries`); it discards what it read.
 */
STATIC size_t
GC_steal_mark_stack_range(AO_t first, AO_t end, mse *local, size_t n_to_get)
{
  AO_t k;
  size_t i = 0;
  size_t n, first_slot, n_before_wrap;
#    ifdef CHECK_MARK_RING
  GC_bool numbers_differ = FALSE;
#    endif

  GC_ASSERT((GC_signed_word)(end - first) > 0
            && (word)(end - first) <= (word)GC_mark_stack_size);
  for (k = first; k != end && i <= n_to_get; ++k) {
    word descr = AO_load(&GC_mark_stack[MARK_RING_INDEX(k)].mse_descr);

    /* If this is a big object, count it as `descr / 256 + 1` objects. */
    ++i;
    if ((descr & GC_DS_TAGS) == GC_DS_LENGTH)
      i += (size_t)(descr >> 8);
  }
  if (AO_load(&GC_first_unclaimed) != first) {
    /* The CAS would fail. */
    return 0;
  }
  n = (size_t)(k - first);
  first_slot = MARK_RING_INDEX(first);
  n_before_wrap = GC_mark_stack_size - first_slot;
  if (n_before_wrap >= n) {
    copy_mark_entries(local, GC_mark_stack + first_slot, n);
  } else {
    copy_mark_entries(local, GC_mark_stack + first_slot, n_before_wrap);
    copy_mark_entries(local + n_before_wrap, GC_mark_stack, n - n_before_wrap);
  }
#    ifdef CHECK_MARK_RING
  {
    AO_t j;

    for (j = first; j != k; ++j) {
      if (AO_load(&GC_mark_ring_numbers[MARK_RING_INDEX(j)]) != j)
        numbers_differ = TRUE;
    }
  }
#    endif
  if (!AO_compare_and_swap_release(&GC_first_unclaimed, first, k))
    return 0;
#    ifdef CHECK_MARK_RING
  /* A slot copied above was written for another entry. */
  GC_ASSERT(!numbers_differ);
  (void)AO_fetch_and_add(&GC_n_claimed, (AO_t)n);
#    endif
  return n;
}

#  else
/*
 * Steal mark stack entries starting at `mse` `low` into mark stack `local`
 * until we either steal `mse` `high`, or we have `n_to_get` entries.
 * Return a pointer to the top of the local mark stack.  `*next` is replaced
 * by a pointer to the next unscanned mark stack entry.
 */
STATIC mse *
GC_steal_mark_stack(mse *low, mse *high, mse *local, size_t n_to_get,
                    mse **next)
{
  mse *p;
  mse *top = local - 1;
  size_t i = 0;

#    ifdef CPPCHECK
  GC_noop1_ptr(local);
#    endif
  GC_ASSERT(ADDR_GE((ptr_t)high, (ptr_t)(low - 1))
            && (word)(high - low + 1) <= GC_mark_stack_size);
  for (p = low; ADDR_GE((ptr_t)high, (ptr_t)p) && i <= n_to_get; ++p) {
    word descr = AO_load(&p->mse_descr);

    if (descr != 0) {
      /* Must be ordered after read of `mse_descr`. */
      AO_store_release_write(&p->mse_descr, 0);
      /*
       * More than one thread may get this entry, but that is only
       * a minor performance problem.
       */
      ++top;
      top->mse_start = p->mse_start;
      top->mse_descr = descr;
      GC_ASSERT((descr & GC_DS_TAGS) != GC_DS_LENGTH /* 0 */
                || descr < GC_greatest_real_heap_addr - GC_least_real_heap_addr
                || GC_least_real_heap_addr + sizeof(ptr_t)
                       >= ADDR(p->mse_start) + descr
                || ADDR(p->mse_start) >= GC_greatest_real_heap_addr);
      /* If this is a big object, count it as `descr / 256 + 1` objects. */
      ++i;
      if ((descr & GC_DS_TAGS) == GC_DS_LENGTH)
        i += (size_t)(descr >> 8);
    }
  }
  *next = p;
  return top;
}
#  endif /* !STEAL_MARK_STACK_RANGES */

/*
 * Copy back a local mark stack.  `low` and `high` are inclusive bounds.
 * If `STEAL_MARK_STACK_RANGES`, the entries are put on the global mark
 * stack used as a ring (see `GC_n_published`); there is room for them if
 * the slots they take hold no unclaimed entries, i.e. the claimed entries
 * leave their slots free.  Otherwise there is room only above
 * `GC_mark_stack_top`.  Without room, the local mark stack is dropped and
 * the mark state invalidated.
 */
STATIC void
GC_return_mark_stack(mse *low, mse *high)
{
#  ifdef STEAL_MARK_STACK_RANGES
  AO_t end, first;
#  else
  mse *my_top;
  mse *my_start;
#  endif
  size_t stack_size;
  GC_bool has_room;

  if (ADDR_LT((ptr_t)high, (ptr_t)low))
    return;
  stack_size = high - low + 1;
  GC_acquire_mark_lock();
#  ifdef STEAL_MARK_STACK_RANGES
  /* Note: the concurrent modification is impossible. */
  end = AO_load(&GC_n_published);
  /*
   * The acquire load orders the writes of the slots below after the reads
   * of the markers that claimed the entries held there before (see
   * `GC_steal_mark_stack_range`).
   */
  first = AO_load_acquire(&GC_first_unclaimed);
  GC_ASSERT((word)(end - first) <= (word)GC_mark_stack_size);
  has_room = (word)(end - first) + stack_size <= (word)GC_mark_stack_size;
#  else
  /* Note: the concurrent modification is impossible. */
  my_top = GC_mark_stack_top;
  my_start = my_top + 1;
  has_room = (word)(my_start - GC_mark_stack + stack_size)
             <= (word)GC_mark_stack_size;
#  endif
  if (!has_room) {
    GC_mark_stack_events.n_no_room++;
    GC_mark_state = MS_INVALID;
    GC_mark_stack_too_small = TRUE;
    /* We drop the local mark stack.  We will fix things later. */
  } else {
#  ifdef STEAL_MARK_STACK_RANGES
    size_t first_slot = MARK_RING_INDEX(end);
    size_t n_before_wrap = GC_mark_stack_size - first_slot;

#    ifdef CHECK_MARK_RING
    {
      AO_t k;

      for (k = end; k != end + (AO_t)stack_size; k++)
        AO_store(&GC_mark_ring_numbers[MARK_RING_INDEX(k)], k);
    }
#    endif
    if (n_before_wrap >= stack_size) {
      copy_mark_entries(GC_mark_stack + first_slot, low, stack_size);
    } else {
      copy_mark_entries(GC_mark_stack + first_slot, low, n_before_wrap);
      copy_mark_entries(GC_mark_stack, low + n_before_wrap,
                        stack_size - n_before_wrap);
    }
    GC_ASSERT(AO_load(&GC_n_published) == end);
    /* Ensures visibility of previously written stack contents. */
    AO_store_release(&GC_n_published, end + (AO_t)stack_size);
#  else
    BCOPY(low, my_start, stack_size * sizeof(mse));
    GC_ASSERT((mse *)GC_cptr_load((volatile ptr_t *)&GC_mark_stack_top)
              == my_top);
    /* Ensures visibility of previously written stack contents. */
    GC_cptr_store_release_write((volatile ptr_t *)&GC_mark_stack_top,
                                (ptr_t)(my_top + stack_size));
#  endif
  }
  GC_release_mark_lock();
  GC_notify_all_marker();
}

#  ifndef N_LOCAL_ITERS
#    define N_LOCAL_ITERS 1
#  endif

#  ifdef STEAL_MARK_STACK_RANGES
/*
 * Same as `has_inactive_helpers()` but without acquiring the mark lock,
 * thus the result might be stale.
 */
static GC_bool
may_have_inactive_helpers(void)
{
  return AO_load(&GC_active_count) < AO_load(&GC_helper_count);
}
#  endif

/*
 * Whether the global mark stack has no entries left for the markers to
 * take, during a parallel mark phase.  Exact if the mark lock is held
 * (then the entries can only be taken, not added), a hint otherwise.
 */
#  ifdef STEAL_MARK_STACK_RANGES
#    define GLOBAL_MARK_STACK_EMPTY()                   \
      ((GC_signed_word)(AO_load(&GC_n_published)        \
                        - AO_load(&GC_first_unclaimed)) \
       <= 0)
#  else
#    define GLOBAL_MARK_STACK_EMPTY()                             \
      ADDR_LT(GC_cptr_load((volatile ptr_t *)&GC_mark_stack_top), \
              GC_cptr_load(&GC_first_nonempty))
#  endif

/*
 * Note: called only when the local and the main mark stacks are both
 * empty.
 */
static GC_bool
has_inactive_helpers(void)
{
  GC_bool res;

#  ifdef STEAL_MARK_STACK_RANGES
  /*
   * `GC_first_unclaimed` is exact in this case, thus the global mark stack
   * is observed empty much more often (e.g. during marking a tree), and
   * we are called after each `GC_mark_from()`; do not contend for the mark
   * lock while all the helpers are likely active.  A stale result only
   * postpones or causes an unneeded split of the local mark stack.
   */
  if (!may_have_inactive_helpers())
    return FALSE;
#  endif
  GC_acquire_mark_lock();
  res = GC_active_count < GC_helper_count;
  GC_release_mark_lock();
  return res;
}

/*
 * Mark from the local mark stack.  On return, the local mark stack
 * is empty.  But this may be achieved by copying the local mark stack
 * back into the global one.  We do not hold the mark lock.
 */
STATIC void
GC_do_local_mark(mse *local_mark_stack, mse *local_top)
{
  unsigned n;

  for (;;) {
    for (n = 0; n < N_LOCAL_ITERS; ++n) {
      local_top = GC_mark_from(local_top, local_mark_stack,
                               local_mark_stack + LOCAL_MARK_STACK_SIZE);
      if (ADDR_LT((ptr_t)local_top, (ptr_t)local_mark_stack))
        return;
      if ((word)(local_top - local_mark_stack) >= LOCAL_MARK_STACK_SIZE / 2) {
        GC_return_mark_stack(local_mark_stack, local_top);
        return;
      }
    }
    if (GLOBAL_MARK_STACK_EMPTY()
        && ADDR_LT((ptr_t)(local_mark_stack + 1), (ptr_t)local_top)
        && has_inactive_helpers()) {
      /*
       * Try to share the load, since the main stack is empty, and the helper
       * threads are waiting for a refill.  The entries near the bottom of
       * the stack are likely to require more work.  Thus we return those,
       * even though it is harder.
       */
      mse *new_bottom = local_mark_stack + (local_top - local_mark_stack) / 2;

      GC_ASSERT(ADDR_LT((ptr_t)local_mark_stack, (ptr_t)new_bottom)
                && ADDR_LT((ptr_t)new_bottom, (ptr_t)local_top));
      GC_return_mark_stack(local_mark_stack, new_bottom - 1);
      memmove(local_mark_stack, new_bottom,
              (local_top - new_bottom + 1) * sizeof(mse));
      local_top -= new_bottom - local_mark_stack;
    }
  }
}

#  ifndef ENTRIES_TO_GET
#    define ENTRIES_TO_GET 5
#  endif

#  ifdef STEAL_MARK_STACK_RANGES
/*
 * The maximum number of entries (counting a big object as several ones)
 * a marker takes from the global mark stack at once.  Should be small
 * enough, relative to `LOCAL_MARK_STACK_SIZE`, not to overflow a local
 * mark stack.
 */
#    ifndef MAX_ENTRIES_TO_GET
#      define MAX_ENTRIES_TO_GET (LOCAL_MARK_STACK_SIZE / 8)
#    endif
#  endif

/*
 * Mark using the local mark stack until the global mark stack is empty and
 * there are no active workers.  Update `GC_first_unclaimed` (or
 * `GC_first_nonempty`) to reflect the progress.  Caller holds the mark
 * lock.  Caller has already incremented `GC_helper_count`; we decrement it,
 * and maintain `GC_active_count`.
 */
STATIC void
GC_mark_local(mse *local_mark_stack, int id)
{
#  ifndef STEAL_MARK_STACK_RANGES
  mse *my_first_nonempty;
#  endif

  AO_store(&GC_active_count, GC_active_count + 1);
#  ifdef STEAL_MARK_STACK_RANGES
  GC_ASSERT((word)(AO_load(&GC_n_published) - AO_load(&GC_first_unclaimed))
            <= (word)GC_mark_stack_size);
#  else
  my_first_nonempty = (mse *)GC_cptr_load(&GC_first_nonempty);
  GC_ASSERT(ADDR_GE((ptr_t)my_first_nonempty, (ptr_t)GC_mark_stack));
  GC_ASSERT(
      ADDR_GE(GC_cptr_load((volatile ptr_t *)&GC_mark_stack_top) + sizeof(mse),
              (ptr_t)my_first_nonempty));
#  endif
  GC_VERBOSE_LOG_PRINTF("Starting mark helper %d\n", id);
  GC_release_mark_lock();
  for (;;) {
    size_t n_on_stack, n_to_get;
    mse *local_top;
    GC_bool looks_empty;
#  ifdef STEAL_MARK_STACK_RANGES
    AO_t first, end;
    size_t n_stolen;

    /*
     * `GC_first_unclaimed` is exact: all the entries numbered below it are
     * claimed, all the entries starting at it are not.  Note: it might be
     * ahead of `GC_n_published` loaded below, then we recheck both holding
     * the mark lock.  It might also be behind the latter by more than the
     * size of the mark stack, if other markers have claimed entries and
     * put new ones on the stack in the meantime, then we load it again.
     */
    first = AO_load(&GC_first_unclaimed);
    end = AO_load_acquire(&GC_n_published);
    looks_empty = (GC_signed_word)(end - first) <= 0;
    if (!looks_empty && (word)(end - first) > (word)GC_mark_stack_size)
      continue;
#  else
    mse *my_top;
    mse *global_first_nonempty = (mse *)GC_cptr_load(&GC_first_nonempty);

    GC_ASSERT(ADDR_GE((ptr_t)my_first_nonempty, (ptr_t)GC_mark_stack)
              && ADDR_GE(GC_cptr_load((volatile ptr_t *)&GC_mark_stack_top)
                             + sizeof(mse),
                         (ptr_t)my_first_nonempty));
    GC_ASSERT(ADDR_GE((ptr_t)global_first_nonempty, (ptr_t)GC_mark_stack));
    if (ADDR_LT((ptr_t)my_first_nonempty, (ptr_t)global_first_nonempty)) {
      my_first_nonempty = global_first_nonempty;
    } else if (ADDR_LT((ptr_t)global_first_nonempty,
                       (ptr_t)my_first_nonempty)) {
      (void)GC_cptr_compare_and_swap(&GC_first_nonempty,
                                     (ptr_t)global_first_nonempty,
                                     (ptr_t)my_first_nonempty);
      /*
       * If this fails, then we just go ahead, without updating
       * `GC_first_nonempty`.
       */
    }
    /*
     * Perhaps we should also update `GC_first_nonempty`, if it is less.
     * But that would require usage of the atomic updates.
     */
    my_top = (mse *)GC_cptr_load_acquire((volatile ptr_t *)&GC_mark_stack_top);
    looks_empty = ADDR_LT((ptr_t)my_top, (ptr_t)my_first_nonempty);
#  endif
    if (looks_empty) {
      GC_acquire_mark_lock();
      /*
       * Note: asynchronous modification is impossible here, since
       * we hold the mark lock.
       */
#  ifdef STEAL_MARK_STACK_RANGES
      end = AO_load(&GC_n_published);
      /*
       * Any claimed range ends at or below the value of `GC_n_published`
       * observed by the claiming marker, and `GC_n_published` does not
       * decrease, thus `GC_first_unclaimed` is not greater than `end` now
       * (but it may still increase).
       */
      first = AO_load(&GC_first_unclaimed);
      GC_ASSERT((word)(end - first) <= (word)GC_mark_stack_size);
      n_on_stack = (size_t)(end - first);
#  else
      my_top = GC_mark_stack_top;
      n_on_stack = my_top - my_first_nonempty + 1;
#  endif
      if (0 == n_on_stack) {
        AO_store(&GC_active_count, GC_active_count - 1);
        GC_ASSERT(GC_active_count <= GC_helper_count);
        /* Other markers may redeposit objects on the stack. */
        if (0 == GC_active_count)
          GC_notify_all_marker();
        while (GC_active_count > 0 && GLOBAL_MARK_STACK_EMPTY()) {
          /*
           * We will be notified if either `GC_active_count` reaches zero,
           * or if more objects are pushed on the global mark stack.
           */
          GC_wait_marker();
        }
        if (0 == GC_active_count && GLOBAL_MARK_STACK_EMPTY()) {
          GC_bool need_to_notify = FALSE;

          /*
           * The above conditions cannot be falsified while we hold
           * the mark lock, since neither `GC_active_count` nor the end of
           * the global mark stack (`GC_n_published` or `GC_mark_stack_top`)
           * can change.  The entries can only be taken asynchronously.
           * Thus we know that both conditions are actually held
           * simultaneously.
           */
#  ifndef GC_NO_IDLE_MARKER_CV
          /*
           * The marking of this phase is complete, so no other marker
           * thread should join the phase.
           */
          GC_help_wanted = FALSE;
#  endif
          AO_store(&GC_helper_count, GC_helper_count - 1);
          if (0 == GC_helper_count)
            need_to_notify = TRUE;
          GC_VERBOSE_LOG_PRINTF("Finished mark helper %d\n", id);
          if (need_to_notify)
            GC_notify_all_marker();
          return;
        }
        /*
         * Else there is something on the stack again, or another helper
         * may push something.
         */
        AO_store(&GC_active_count, GC_active_count + 1);
        GC_ASSERT(GC_active_count > 0);
        GC_release_mark_lock();
        continue;
      } else {
        GC_release_mark_lock();
      }
    } else {
#  ifdef STEAL_MARK_STACK_RANGES
      n_on_stack = (size_t)(end - first);
#  else
      n_on_stack = my_top - my_first_nonempty + 1;
#  endif
    }
    n_to_get = ENTRIES_TO_GET;
    if (n_on_stack < 2 * ENTRIES_TO_GET) {
      n_to_get = 1;
    }
#  ifdef STEAL_MARK_STACK_RANGES
    else {
      /*
       * Take a fair share of the global mark stack (half of it divided by
       * the number of markers of the phase), not to steal so often in case
       * of a huge mark stack (e.g. after pushing all the marked objects of
       * the dirty pages in a generational collection).
       */
      size_t fair_share = n_on_stack / (2 * (size_t)GC_phase_markers);

      if (fair_share > MAX_ENTRIES_TO_GET)
        fair_share = MAX_ENTRIES_TO_GET;
      if (fair_share > n_to_get)
        n_to_get = fair_share;
    }
    n_stolen
        = GC_steal_mark_stack_range(first, end, local_mark_stack, n_to_get);
    if (0 == n_stolen) {
      /* Another marker has claimed the entries; retry. */
      continue;
    }
    local_top = local_mark_stack + n_stolen - 1;
#  else
    local_top
        = GC_steal_mark_stack(my_first_nonempty, my_top, local_mark_stack,
                              n_to_get, &my_first_nonempty);
    GC_ASSERT(ADDR_GE((ptr_t)my_first_nonempty, (ptr_t)GC_mark_stack)
              && ADDR_GE(GC_cptr_load((volatile ptr_t *)&GC_mark_stack_top)
                             + sizeof(mse),
                         (ptr_t)my_first_nonempty));
#  endif
    GC_do_local_mark(local_mark_stack, local_top);
  }
}

/*
 * Perform parallel mark.  We hold the allocator lock, but not the mark lock.
 * Currently runs until the mark stack is empty.
 */
STATIC void
GC_do_parallel_mark(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(NULL == GC_next_mark_stack);
  GC_acquire_mark_lock();
  GC_ASSERT(!GC_help_wanted);
  GC_ASSERT(0 == GC_active_count && 0 == GC_helper_count);
  GC_VERBOSE_LOG_PRINTF("Starting marking for mark phase number %lu\n",
                        (unsigned long)GC_mark_no);

  GC_ASSERT(GC_phase_markers > 1
            && GC_phase_markers <= (unsigned)GC_markers_m1 + 1);
#  ifdef STEAL_MARK_STACK_RANGES
  /*
   * The entries pushed before the phase are numbered from zero; they are
   * in the slots of the same indices.  No claim of the previous phase is
   * in progress (its markers have all finished), thus `GC_first_unclaimed`
   * may be written by a plain store here.
   */
  GC_ASSERT((GC_mark_stack_size & (GC_mark_stack_size - 1)) == 0);
  AO_store(&GC_first_unclaimed, 0);
  AO_store(&GC_n_published, (AO_t)(GC_mark_stack_top + 1 - GC_mark_stack));
#    ifdef CHECK_MARK_RING
  {
    AO_t k;
    AO_t n = AO_load(&GC_n_published);

    for (k = 0; k < n; k++)
      AO_store(&GC_mark_ring_numbers[k], k);
    AO_store(&GC_n_claimed, 0);
  }
#    endif
#  else
  GC_cptr_store(&GC_first_nonempty, (ptr_t)GC_mark_stack);
#  endif
  AO_store(&GC_active_count, 0);
  AO_store(&GC_helper_count, 1);
  GC_help_wanted = TRUE;
  /* Wake up as many helpers as the phase has besides this thread. */
  GC_notify_idle_markers(GC_phase_markers - 1);
  GC_mark_local(GC_main_local_mark_stack, 0);
#  ifndef GC_NO_IDLE_MARKER_CV
  /* Cleared by the first marker which found the marking complete. */
  GC_ASSERT(!GC_help_wanted);
#  endif
  GC_help_wanted = FALSE;
  /* Done; clean up. */
  while (GC_helper_count > 0) {
    GC_wait_marker();
  }
  /* `GC_helper_count` cannot be incremented while not `GC_help_wanted`. */
#  ifdef STEAL_MARK_STACK_RANGES
  /* Every entry put on the global mark stack has been claimed once. */
  GC_ASSERT(AO_load(&GC_first_unclaimed) == AO_load(&GC_n_published));
#    ifdef CHECK_MARK_RING
  GC_ASSERT(AO_load(&GC_n_claimed) == AO_load(&GC_n_published));
#    endif
#  else
  GC_ASSERT(ADDR_LT((ptr_t)GC_mark_stack_top, GC_first_nonempty));
#  endif
  GC_VERBOSE_LOG_PRINTF("Finished marking for mark phase number %lu\n",
                        (unsigned long)GC_mark_no);
  GC_mark_no++;
  GC_release_mark_lock();
#  ifdef GC_NO_IDLE_MARKER_CV
  GC_notify_all_marker();
#  else
  /*
   * The marker threads waiting for a mark phase are not woken up here.
   * Such a thread joins a phase only if `GC_help_wanted` is set, which is
   * not the case now.  A thread which sleeps through the increment of
   * `GC_mark_no` is woken up at the start of the next phase, returns from
   * `GC_help_marker()` (as its mark number is behind), and resynchronizes
   * with `GC_mark_no` in `GC_mark_thread()`, the only caller of
   * `GC_help_marker()`.
   */
#  endif
}

GC_INNER void
GC_help_marker(word my_mark_no)
{
#  define my_id my_id_mse.mse_descr
  /*
   * Put `my_id` inside the structure to keep `local_mark_stack` aligned
   * explicitly.
   */
  mse my_id_mse;
  mse local_mark_stack[LOCAL_MARK_STACK_SIZE];
  /*
   * Note: `local_mark_stack` is quite big (128 KiB if `HBLKSIZE` is 8 KB,
   * more if `HBLKSIZE` is bigger, see `DEFAULT_STACK_MAYBE_SMALL`).
   */

  GC_ASSERT(I_DONT_HOLD_LOCK());
  GC_ASSERT(GC_parallel);
  while (GC_mark_no < my_mark_no
         || (!GC_help_wanted && GC_mark_no == my_mark_no)) {
    GC_wait_idle_marker();
  }
  my_id = GC_helper_count;
  if (GC_mark_no != my_mark_no || my_id >= (word)GC_phase_markers) {
    /*
     * The mark cycle is done, or the phase has all its markers already
     * (see `markers_for_phase()`); in the latter case this thread waits
     * for the next phase.
     */
    return;
  }
  AO_store(&GC_helper_count, my_id + 1);
  GC_mark_local(local_mark_stack, (int)my_id);
  /* `GC_mark_local` decrements `GC_helper_count`. */
#  undef my_id
}

#endif /* PARALLEL_MARK */

/*
 * The size in bytes of the memory of a mark stack of `n` entries.  It is
 * at least `MINHINCR * HBLKSIZE`, which `GC_scratch_alloc()` gets from
 * the OS directly, without a warning if this fails, as a mark stack may
 * grow with the world stopped.  The memory after the `n` entries (of
 * `MARK_STACK_ENTRY_BYTES` each) is not used.  Their size in bytes must
 * fit in `size_t`.
 */
static size_t
mark_stack_bytes(size_t n)
{
  size_t bytes = n * MARK_STACK_ENTRY_BYTES;

  return bytes < (size_t)MINHINCR * HBLKSIZE ? (size_t)MINHINCR * HBLKSIZE
                                             : bytes;
}

/*
 * Make `new_stack`, of `n` entries, the global mark stack, and put the
 * old one, if any, on the list of the outgrown mark stacks (to be
 * recycled into the heap by `GC_after_marking()`).  The mark stack top
 * is not set.
 */
static void
set_mark_stack(mse *new_stack, size_t n)
{
#ifdef GWW_VDB
  /*
   * Do not recycle a stack segment obtained with the wrong flags.
   * Win32 `GetWriteWatch` requires the right kind of memory.
   */
  GC_bool recycle_old = !GC_auto_incremental || GC_incremental_at_stack_alloc;

  GC_incremental_at_stack_alloc = GC_auto_incremental;
#endif
  GC_ASSERT(I_HOLD_LOCK());
  if (GC_mark_stack != NULL) {
#ifdef GWW_VDB
    if (recycle_old)
#endif
    {
      add_outgrown_mark_stack(GC_mark_stack, GC_mark_stack_size);
    }
    GC_mark_stack_events.n_grown++;
    GC_mark_stack_events.grown_size = n;
  }
  GC_mark_stack = new_stack;
  GC_mark_stack_size = n;
  /* FIXME: Do we need some way to reset `GC_mark_stack_size`? */
  GC_mark_stack_limit = new_stack + n;
#ifdef CHECK_MARK_RING
  GC_mark_ring_numbers = (volatile AO_t *)GC_mark_stack_limit;
#endif
}

/*
 * Allocate or reallocate space for mark stack of size `n` entries.
 * The old mark stack is recycled into the heap by `GC_after_marking()`,
 * which also warns if a reallocation failed (the old mark stack is kept
 * then).  If `STEAL_MARK_STACK_RANGES`, `n` is rounded up to a power of
 * two, as a parallel mark phase uses the mark stack as a ring.
 */
static void
alloc_mark_stack(size_t n)
{
  mse *new_stack;

  GC_ASSERT(I_HOLD_LOCK());
  /* The mark stack is empty, thus no entries wait in the next one. */
  GC_ASSERT(NULL == GC_next_mark_stack);
#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES)
  if ((n & (n - 1)) != 0) {
    size_t p2 = 1;

    while (p2 < n && p2 <= GC_SIZE_MAX / 2)
      p2 <<= 1;
    /* A size that cannot be rounded up fails below. */
    n = p2 < n ? GC_SIZE_MAX : p2;
  }
#endif
  if (UNLIKELY(n > GC_SIZE_MAX / MARK_STACK_ENTRY_BYTES)) {
    /* The size in bytes does not fit in `size_t`. */
    new_stack = NULL;
  } else {
    new_stack = (mse *)GC_scratch_alloc(mark_stack_bytes(n));
  }
  GC_mark_stack_too_small = FALSE;
  if (new_stack != NULL) {
    set_mark_stack(new_stack, n);
  } else if (GC_mark_stack != NULL) {
    GC_mark_stack_events.failed_size = n;
  } else {
    GC_err_printf("No space for mark stack\n");
    EXIT();
  }
  GC_mark_stack_top = GC_mark_stack - 1;
}

GC_INNER void
GC_switch_to_next_mark_stack(void)
{
  mse *next = GC_next_mark_stack;
  size_t n = (size_t)(GC_mark_stack_top + 1 - GC_mark_stack);

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(next != NULL);
  GC_ASSERT(ADDR_GE((ptr_t)(GC_mark_stack_top + 1), (ptr_t)GC_mark_stack));
  GC_ASSERT(n <= GC_next_mark_stack_size - GC_n_moved_entries);
  BCOPY(GC_mark_stack, next + GC_n_moved_entries, n * sizeof(mse));
  n += GC_n_moved_entries;
  GC_next_mark_stack = NULL;
  GC_n_moved_entries = 0;
  set_mark_stack(next, GC_next_mark_stack_size);
  GC_mark_stack_top = next + n - 1;
}

/* Log the events recorded in `GC_mark_stack_events`, and clear them. */
static void
log_mark_stack_events(void)
{
#ifdef PARALLEL_MARK
  word n_local = (word)AO_load(&GC_n_local_mark_stack_overflows);

  if (n_local != 0)
    AO_store(&GC_n_local_mark_stack_overflows, 0);
#endif
  if (UNLIKELY(GC_mark_stack_events.failed_size != 0)) {
    WARN("Failed to grow mark stack to %" WARN_PRIuPTR " frames\n",
         (word)GC_mark_stack_events.failed_size);
  }
  if (UNLIKELY(GC_print_stats)) {
    if (GC_mark_stack_events.n_overflows != 0) {
      GC_log_printf("Mark stack overflow (%lu times, at %lu entries)\n",
                    (unsigned long)GC_mark_stack_events.n_overflows,
                    (unsigned long)GC_mark_stack_events.overflow_size);
    }
    if (GC_mark_stack_events.n_moves != 0) {
      GC_log_printf("Moved %lu entries of full mark stack to a larger one"
                    " (%lu times)\n",
                    (unsigned long)GC_mark_stack_events.moved_entries,
                    (unsigned long)GC_mark_stack_events.n_moves);
    }
#ifdef PARALLEL_MARK
    if (n_local != 0) {
      GC_log_printf("Local mark stack overflow (%lu times)\n",
                    (unsigned long)n_local);
    }
#endif
    if (GC_mark_stack_events.n_no_room != 0) {
      GC_log_printf("No room to copy back mark stack (%lu times)\n",
                    (unsigned long)GC_mark_stack_events.n_no_room);
    }
    if (GC_mark_stack_events.n_grown != 0) {
      GC_log_printf("Grew mark stack to %lu frames (%lu times)\n",
                    (unsigned long)GC_mark_stack_events.grown_size,
                    (unsigned long)GC_mark_stack_events.n_grown);
    }
  }
  BZERO(&GC_mark_stack_events, sizeof(GC_mark_stack_events));
}

GC_INNER void
GC_after_marking(void)
{
  GC_ASSERT(I_HOLD_LOCK());
#ifndef GC_DISABLE_INCREMENTAL
  if (GC_rescue_pass_ended) {
    GC_rescue_pass_ended = FALSE;
    GC_COND_LOG_PRINTF("Marked from %lu dirty pages\n",
                       (unsigned long)GC_n_rescued_pages);
  }
#endif
#ifdef PARALLEL_MARK
  log_marker_decisions();
#endif
  log_mark_stack_events();
  while (GC_outgrown_mark_stacks != NULL) {
    struct outgrown_mark_stack *old = GC_outgrown_mark_stacks;

    GC_outgrown_mark_stacks = old->next;
    GC_scratch_recycle_inner(old, old->bytes);
  }
}

GC_INNER void
GC_mark_init(void)
{
  GC_STATIC_ASSERT(INITIAL_MARK_STACK_SIZE
                   <= GC_SIZE_MAX / MARK_STACK_ENTRY_BYTES);
  alloc_mark_stack(INITIAL_MARK_STACK_SIZE);
}

GC_API void GC_CALL
GC_push_all(void *bottom, void *top)
{
#if defined(WRAP_MARK_SOME) && defined(PARALLEL_MARK)
  /*
   * `GC_mark_some` recovers from a fault only on the thread holding the
   * allocator lock, and not while the parallel markers run.  The range
   * may disappear (e.g. a thread stack, or a range pushed by the client),
   * thus scan it immediately, so that only heap objects get onto the mark
   * stack.
   */
  if (GC_parallel) {
    GC_push_conditional_eager(bottom, top, TRUE);
    return;
  }
#endif
  GC_mark_stack_top
      = GC_ms_push_all(bottom, top, GC_mark_stack_top, GC_mark_stack_limit);
}

GC_API struct GC_ms_entry *GC_CALL
GC_ms_push_all(void *bottom, void *top, struct GC_ms_entry *mark_stack_top,
               struct GC_ms_entry *mark_stack_limit)
{
  word length;

  bottom = PTR_ALIGN_UP((ptr_t)bottom, ALIGNMENT);
  top = PTR_ALIGN_DOWN((ptr_t)top, ALIGNMENT);
  if (ADDR_GE((ptr_t)bottom, (ptr_t)top))
    return mark_stack_top;

  length = (word)((ptr_t)top - (ptr_t)bottom);
#if GC_DS_TAGS > ALIGNMENT - 1
  length = (length + GC_DS_TAGS) & ~(word)GC_DS_TAGS; /*< round up */
#endif
  return GC_ms_push_obj_descr(bottom, length | GC_DS_LENGTH, mark_stack_top,
                              mark_stack_limit);
}

GC_API struct GC_ms_entry *GC_CALL
GC_ms_push_obj_descr(void *obj, GC_word descr,
                     struct GC_ms_entry *mark_stack_top,
                     struct GC_ms_entry *mark_stack_limit)
{
  mark_stack_top++;
  if (ADDR_GE((ptr_t)mark_stack_top, (ptr_t)mark_stack_limit)) {
    mark_stack_top
        = GC_signal_mark_stack_overflow(mark_stack_top, mark_stack_limit);
  }
  mark_stack_top->mse_start = (ptr_t)obj;
  mark_stack_top->mse_descr = descr;
  return mark_stack_top;
}

GC_API void GC_CALL
GC_push_obj_descr(void *obj, GC_word descr)
{
  GC_mark_stack_top = GC_ms_push_obj_descr(obj, descr, GC_mark_stack_top,
                                           GC_mark_stack_limit);
}

#ifndef GC_DISABLE_INCREMENTAL

typedef GC_bool (*dirty_func_t)(const struct hblk *);

/*
 * Analogous to `GC_push_all`, but push only those pages `h` with
 * `dirty_fn(h) != 0`.  We use `GC_push_all` to actually push the block.
 * Used both to selectively push dirty pages, or to push a block in
 * a piecemeal fashion, to allow for more marking concurrency.
 * Will not overflow mark stack if `GC_push_all` pushes a small fixed
 * number of entries.  (This is invoked only if `GC_push_all` pushes
 * a single entry, or if it marks each object before pushing it, thus
 * ensuring progress in the event of a stack overflow.)
 */
STATIC void
GC_push_selected(ptr_t bottom, ptr_t top, dirty_func_t dirty_fn)
{
  struct hblk *h;

  bottom = PTR_ALIGN_UP(bottom, ALIGNMENT);
  top = PTR_ALIGN_DOWN(top, ALIGNMENT);
  if (ADDR_GE(bottom, top))
    return;

  h = HBLKPTR(bottom + HBLKSIZE); /*< round up to the next block */
  if (ADDR_GE((ptr_t)h, top)) {
    if ((*dirty_fn)(h - 1)) {
      GC_push_all(bottom, top);
    }
    return;
  }
  if ((*dirty_fn)(h - 1)) {
    if ((word)(GC_mark_stack_top - GC_mark_stack)
        > 3 * GC_mark_stack_size / 4) {
      GC_push_all(bottom, top);
      return;
    }
    GC_push_all(bottom, h);
  }

  while (ADDR_GE(top, (ptr_t)(h + 1))) {
    if ((*dirty_fn)(h)) {
      if ((word)(GC_mark_stack_top - GC_mark_stack)
          > 3 * GC_mark_stack_size / 4) {
        /* Danger of mark stack overflow. */
        GC_push_all(h, top);
        return;
      } else {
        GC_push_all(h, h + 1);
      }
    }
    h++;
  }

  if ((ptr_t)h != top && (*dirty_fn)(h)) {
    GC_push_all(h, top);
  }
}

GC_API void GC_CALL
GC_push_conditional(void *bottom, void *top, int all)
{
  if (!all) {
    GC_push_selected((ptr_t)bottom, (ptr_t)top, GC_page_was_dirty);
  } else {
#  ifdef PROC_VDB
    if (GC_auto_incremental) {
      /* Pages that were never dirtied cannot contain pointers. */
      GC_push_selected((ptr_t)bottom, (ptr_t)top, GC_page_was_ever_dirty);
    } else
#  endif
    /* else */ {
      GC_push_all(bottom, top);
    }
  }
}

#  if !defined(NO_VDB_FOR_STATIC_ROOTS) && !defined(PROC_VDB)
/*
 * Same as `GC_page_was_dirty` but `h` is allowed to point to some page
 * in the registered static roots only.  Not used if the manual VDB is on.
 */
STATIC GC_bool
GC_static_page_was_dirty(const struct hblk *h)
{
  return get_pht_entry_from_index(GC_grungy_pages, PHT_HASH(h));
}
#  endif

#else
GC_API void GC_CALL
GC_push_conditional(void *bottom, void *top, int all)
{
  UNUSED_ARG(all);
  GC_push_all(bottom, top);
}
#endif /* GC_DISABLE_INCREMENTAL */

#if !defined(NO_VDB_FOR_STATIC_ROOTS) || defined(USE_PROC_FOR_LIBRARIES)
GC_INNER void
GC_push_conditional_static(void *bottom, void *top, GC_bool all)
{
#  if defined(USE_PROC_FOR_LIBRARIES) && defined(WRAP_MARK_SOME)
  if (LIKELY(ADDR_LT((ptr_t)bottom, (ptr_t)top))) {
    CHECK_MEMORY_READ(bottom);
    CHECK_MEMORY_READ((ptr_t)top - 1);
  }
#  endif
#  ifdef PROC_VDB
  /*
   * Just redirect to the generic routine because `PROC_VDB`
   * implementation gets the dirty bits map for the whole process memory.
   */
  GC_push_conditional(bottom, top, all);
#  else
#    ifndef NO_VDB_FOR_STATIC_ROOTS
  if (!all && GC_is_vdb_for_static_roots()) {
    GC_push_selected((ptr_t)bottom, (ptr_t)top, GC_static_page_was_dirty);
  } else
#    endif
  /* else */ {
    GC_push_all(bottom, top);
    UNUSED_ARG(all);
  }
#  endif
}
#endif

#if defined(DARWIN) && defined(THREADS)
void
GC_push_one(word p)
{
  GC_PUSH_ONE_STACK((ptr_t)p, MARKED_FROM_REGISTER);
}
#endif /* DARWIN && THREADS */

#if defined(GC_WIN32_THREADS)
GC_INNER void
GC_push_many_regs(const word *regs, unsigned count)
{
  unsigned i;

  for (i = 0; i < count; i++)
    GC_PUSH_ONE_STACK((ptr_t)regs[i], MARKED_FROM_REGISTER);
}
#endif /* GC_WIN32_THREADS */

GC_API struct GC_ms_entry *GC_CALL
GC_mark_and_push(void *obj, mse *mark_stack_top, mse *mark_stack_limit,
                 void **src)
{
  hdr *hhdr;

  PREFETCH_CANDIDATE(obj);
  GET_HDR(obj, hhdr);
  if ((UNLIKELY(IS_FORWARDING_ADDR_OR_NIL(hhdr))
       && (!GC_all_interior_pointers
           || NULL == (hhdr = GC_find_header(GC_base(obj)))))
      || UNLIKELY(HBLK_IS_FREE(hhdr))) {
    GC_ADD_TO_BLACK_LIST_NORMAL((ptr_t)obj, (ptr_t)src);
    return mark_stack_top;
  }
  return GC_ms_push_contents_hdr((ptr_t)obj, hhdr, mark_stack_top,
                                 mark_stack_limit, (ptr_t)src, TRUE);
}

GC_ATTR_NO_SANITIZE_ADDR
GC_INNER void
#if defined(PRINT_BLACK_LIST) || defined(KEEP_BACK_PTRS)
GC_mark_and_push_stack(ptr_t p, ptr_t source)
#else
GC_mark_and_push_stack(ptr_t p)
#  define source ((ptr_t)NULL)
#endif
{
  hdr *hhdr;
  ptr_t r = p;

  PREFETCH_CANDIDATE(p);
  GET_HDR(p, hhdr);
  if (UNLIKELY(IS_FORWARDING_ADDR_OR_NIL(hhdr))) {
    if (NULL == hhdr || (r = (ptr_t)GC_base(p)) == NULL
        || (hhdr = HDR(r)) == NULL) {
      GC_ADD_TO_BLACK_LIST_STACK(p, source);
      return;
    }
  }
  if (UNLIKELY(HBLK_IS_FREE(hhdr))) {
    GC_ADD_TO_BLACK_LIST_NORMAL(p, source);
    return;
  }
#ifdef THREADS
  /*
   * Pointer is on the stack.  We may have dirtied the object it points to,
   * but have not called `GC_dirty` yet.
   */
  GC_dirty(p); /*< entire object */
#endif
  GC_mark_stack_top = GC_ms_push_contents_hdr(
      r, hhdr, GC_mark_stack_top, GC_mark_stack_limit, source, FALSE);
  /*
   * We silently ignore pointers to near the end of a block, which is
   * very mildly suboptimal.
   */
  /* FIXME: We should probably add a header word to address this. */
#undef source
}

#ifdef TRACE_BUF
#  ifndef TRACE_ENTRIES
#    define TRACE_ENTRIES 1000
#  endif

struct trace_entry {
  const char *caller_fn_name;
  word gc_no;
  word bytes_allocd;
  GC_hidden_pointer arg1;
  GC_hidden_pointer arg2;
} GC_trace_buf[TRACE_ENTRIES] = { { (const char *)NULL, 0, 0, 0, 0 } };

void
GC_add_trace_entry(const char *caller_fn_name, ptr_t arg1, ptr_t arg2)
{
  size_t i = GC_trace_buf_pos;

  GC_trace_buf[i].caller_fn_name = caller_fn_name;
  GC_trace_buf[i].gc_no = GC_gc_no;
  GC_trace_buf[i].bytes_allocd = GC_bytes_allocd;
  GC_trace_buf[i].arg1 = GC_HIDE_POINTER(arg1);
  GC_trace_buf[i].arg2 = GC_HIDE_POINTER(arg2);
  i++;
  if (i >= TRACE_ENTRIES)
    i = 0;
  GC_trace_buf_pos = i;
}

GC_API void GC_CALL
GC_print_trace_inner(GC_word gc_no)
{
  size_t i;

  for (i = GC_trace_buf_pos;; i--) {
    struct trace_entry *p;

    if (0 == i)
      i = TRACE_ENTRIES;
    p = &GC_trace_buf[i - 1];
    /*
     * Compare `gc_no` values (`p->gc_no` is less than given `gc_no`)
     * taking into account that the counter may overflow.
     */
    if (((p->gc_no - gc_no) & SIGNB) != 0 || NULL == p->caller_fn_name) {
      return;
    }
    GC_printf("Trace:%s (gc:%lu, bytes:%lu) %p, %p\n", p->caller_fn_name,
              (unsigned long)p->gc_no, (unsigned long)p->bytes_allocd,
              GC_REVEAL_POINTER(p->arg1), GC_REVEAL_POINTER(p->arg2));
    if (i == GC_trace_buf_pos + 1)
      break;
  }
  GC_printf("Trace incomplete\n");
}

GC_API void GC_CALL
GC_print_trace(GC_word gc_no)
{
  READER_LOCK();
  GC_print_trace_inner(gc_no);
  READER_UNLOCK();
}
#endif /* TRACE_BUF */

GC_ATTR_NO_SANITIZE_ADDR_MEM_THREAD
GC_API void GC_CALL
GC_push_all_eager(void *bottom, void *top)
{
  REGISTER ptr_t current_p;
  REGISTER word lim_addr;
  REGISTER ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  REGISTER ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
#define GC_greatest_plausible_heap_addr greatest_ha
#define GC_least_plausible_heap_addr least_ha

  if (NULL == top)
    return;
  /* Check all pointers in range and push if they appear to be valid. */
  current_p = PTR_ALIGN_UP((ptr_t)bottom, ALIGNMENT);
  lim_addr = ADDR(PTR_ALIGN_DOWN((ptr_t)top, ALIGNMENT)) - sizeof(ptr_t);
#ifdef CHERI_PURECAP
  {
    word cap_limit = cheri_base_get(current_p) + cheri_length_get(current_p);

    if (lim_addr >= cap_limit)
      lim_addr = cap_limit - sizeof(ptr_t);
  }
#endif
  for (; ADDR(current_p) <= lim_addr; current_p += ALIGNMENT) {
    REGISTER ptr_t q;

    LOAD_PTR_OR_CONTINUE(q, current_p);
    GC_PUSH_ONE_STACK(q, current_p);
  }
#undef GC_greatest_plausible_heap_addr
#undef GC_least_plausible_heap_addr
}

#if !defined(NEED_FIXUP_POINTER) && !defined(NO_ALL_INTERIOR_POINTERS)   \
    && (!defined(STACK_NOT_SCANNED) || defined(IA64) || defined(THREADS) \
        || (defined(EMSCRIPTEN) && defined(EMSCRIPTEN_ASYNCIFY)))
GC_INNER void
GC_push_all_stack(void *bottom, void *top)
{
  GC_ASSERT(I_HOLD_LOCK());
  if (GC_all_interior_pointers
#  ifdef THREADS
      /*
       * In the incremental mode the marking may stop, and the world be
       * restarted, before a pushed range is scanned.  The stack of a thread
       * which exits meanwhile may be unmapped by then, thus scan stacks
       * at once in this mode, whatever the virtual dirty bits.
       */
      && !GC_incremental
#  endif
      && ADDR_LT((ptr_t)GC_mark_stack_top,
                 (ptr_t)(GC_mark_stack_limit - INITIAL_MARK_STACK_SIZE / 8))) {
    GC_push_all(bottom, top);
  } else {
    GC_push_all_eager(bottom, top);
  }
}
#endif

#if defined(WRAP_MARK_SOME) && defined(PARALLEL_MARK)
GC_ATTR_NO_SANITIZE_ADDR_MEM_THREAD
GC_INNER void
GC_push_conditional_eager(void *bottom, void *top, GC_bool all)
{
  REGISTER ptr_t current_p;
  REGISTER ptr_t lim;
  REGISTER ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  REGISTER ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
#  define GC_greatest_plausible_heap_addr greatest_ha
#  define GC_least_plausible_heap_addr least_ha

  if (NULL == top)
    return;

  /* TODO: If not `all`, then scan only dirty pages. */
  (void)all;

  current_p = PTR_ALIGN_UP((ptr_t)bottom, ALIGNMENT);
  lim = PTR_ALIGN_DOWN((ptr_t)top, ALIGNMENT) - sizeof(ptr_t);
  for (; ADDR_GE(lim, current_p); current_p += ALIGNMENT) {
    REGISTER ptr_t q;

    LOAD_PTR_OR_CONTINUE(q, current_p);
    GC_PUSH_ONE_HEAP(q, current_p, GC_mark_stack_top);
  }
#  undef GC_greatest_plausible_heap_addr
#  undef GC_least_plausible_heap_addr
}
#endif /* WRAP_MARK_SOME && PARALLEL_MARK */

#if !defined(SMALL_CONFIG) && !defined(USE_MARK_BYTES) \
    && !defined(MARK_BIT_PER_OBJ) && GC_GRANULE_PTRS <= 4
#  define USE_PUSH_MARKED_ACCELERATORS
#  if GC_GRANULE_PTRS == 1
#    define PUSH_GRANULE(q)                                 \
      do {                                                  \
        ptr_t q_contents = (q)[0];                          \
        GC_PUSH_ONE_HEAP(q_contents, q, GC_mark_stack_top); \
      } while (0)
#  elif GC_GRANULE_PTRS == 2
#    define PUSH_GRANULE(q)                                       \
      do {                                                        \
        ptr_t q_contents = (q)[0];                                \
        GC_PUSH_ONE_HEAP(q_contents, q, GC_mark_stack_top);       \
        q_contents = (q)[1];                                      \
        GC_PUSH_ONE_HEAP(q_contents, (q) + 1, GC_mark_stack_top); \
      } while (0)
#  else
#    define PUSH_GRANULE(q)                                       \
      do {                                                        \
        ptr_t q_contents = (q)[0];                                \
        GC_PUSH_ONE_HEAP(q_contents, q, GC_mark_stack_top);       \
        q_contents = (q)[1];                                      \
        GC_PUSH_ONE_HEAP(q_contents, (q) + 1, GC_mark_stack_top); \
        q_contents = (q)[2];                                      \
        GC_PUSH_ONE_HEAP(q_contents, (q) + 2, GC_mark_stack_top); \
        q_contents = (q)[3];                                      \
        GC_PUSH_ONE_HEAP(q_contents, (q) + 3, GC_mark_stack_top); \
      } while (0)
#  endif

/*
 * Push all objects reachable from marked objects in the given block
 * containing objects of size 1 granule.
 */
GC_ATTR_NO_SANITIZE_THREAD
STATIC void
GC_push_marked1(struct hblk *h, const hdr *hhdr)
{
  const word *mark_word_addr
      = (word *)CAST_AWAY_VOLATILE_PVOID(hhdr->hb_marks);
  ptr_t *p;
  ptr_t plim;

  /*
   * Allow registers to be used for some frequently accessed global variables.
   * Otherwise aliasing issues are likely to prevent that.
   */
  ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
  mse *mark_stack_top = GC_mark_stack_top;
  mse *mark_stack_limit = GC_mark_stack_limit;

#  undef GC_mark_stack_top
#  undef GC_mark_stack_limit
#  define GC_mark_stack_top mark_stack_top
#  define GC_mark_stack_limit mark_stack_limit
#  define GC_greatest_plausible_heap_addr greatest_ha
#  define GC_least_plausible_heap_addr least_ha

  p = (ptr_t *)h->hb_body;
  plim = (ptr_t)h + HBLKSIZE;

  /* Go through all granules in block. */
  while (ADDR_LT((ptr_t)p, plim)) {
    word mark_word = *mark_word_addr++;
    ptr_t *q;

    for (q = p; mark_word != 0; mark_word >>= 1) {
      if ((mark_word & 1) != 0)
        PUSH_GRANULE(q);
      q += GC_GRANULE_PTRS;
    }
    p += CPP_WORDSZ * GC_GRANULE_PTRS;
  }

#  undef GC_greatest_plausible_heap_addr
#  undef GC_least_plausible_heap_addr
#  undef GC_mark_stack_top
#  undef GC_mark_stack_limit
#  define GC_mark_stack_limit GC_arrays._mark_stack_limit
#  define GC_mark_stack_top GC_arrays._mark_stack_top
  GC_mark_stack_top = mark_stack_top;
}

#  ifndef UNALIGNED_PTRS
/*
 * Push all objects reachable from marked objects in the given block
 * of two-granule objects.
 */
GC_ATTR_NO_SANITIZE_THREAD
STATIC void
GC_push_marked2(struct hblk *h, const hdr *hhdr)
{
  const word *mark_word_addr
      = (word *)CAST_AWAY_VOLATILE_PVOID(hhdr->hb_marks);
  ptr_t *p;
  ptr_t plim;
  ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
  mse *mark_stack_top = GC_mark_stack_top;
  mse *mark_stack_limit = GC_mark_stack_limit;

#    undef GC_mark_stack_top
#    undef GC_mark_stack_limit
#    define GC_mark_stack_top mark_stack_top
#    define GC_mark_stack_limit mark_stack_limit
#    define GC_greatest_plausible_heap_addr greatest_ha
#    define GC_least_plausible_heap_addr least_ha

  p = (ptr_t *)h->hb_body;
  plim = (ptr_t)h + HBLKSIZE;

  /* Go through all granules in block. */
  while (ADDR_LT((ptr_t)p, plim)) {
    word mark_word = *mark_word_addr++;
    ptr_t *q;

    for (q = p; mark_word != 0; mark_word >>= 2) {
      if (mark_word & 1) {
        PUSH_GRANULE(q);
        PUSH_GRANULE(q + GC_GRANULE_PTRS);
      }
      q += 2 * GC_GRANULE_PTRS;
    }
    p += CPP_WORDSZ * GC_GRANULE_PTRS;
  }

#    undef GC_greatest_plausible_heap_addr
#    undef GC_least_plausible_heap_addr
#    undef GC_mark_stack_top
#    undef GC_mark_stack_limit
#    define GC_mark_stack_limit GC_arrays._mark_stack_limit
#    define GC_mark_stack_top GC_arrays._mark_stack_top
  GC_mark_stack_top = mark_stack_top;
}

#    if GC_GRANULE_PTRS < 4
/*
 * Push all objects reachable from marked objects in the given block of
 * four-granule objects.  There is a risk of mark stack overflow here.
 * But we handle that.  And only unmarked objects get pushed, so it is
 * not very likely.
 */
GC_ATTR_NO_SANITIZE_THREAD
STATIC void
GC_push_marked4(struct hblk *h, const hdr *hhdr)
{
  const word *mark_word_addr
      = (word *)CAST_AWAY_VOLATILE_PVOID(hhdr->hb_marks);
  ptr_t *p;
  ptr_t plim;
  ptr_t greatest_ha = (ptr_t)GC_greatest_plausible_heap_addr;
  ptr_t least_ha = (ptr_t)GC_least_plausible_heap_addr;
  mse *mark_stack_top = GC_mark_stack_top;
  mse *mark_stack_limit = GC_mark_stack_limit;

#      undef GC_mark_stack_top
#      undef GC_mark_stack_limit
#      define GC_mark_stack_top mark_stack_top
#      define GC_mark_stack_limit mark_stack_limit
#      define GC_greatest_plausible_heap_addr greatest_ha
#      define GC_least_plausible_heap_addr least_ha

  p = (ptr_t *)h->hb_body;
  plim = (ptr_t)h + HBLKSIZE;

  /* Go through all granules in block. */
  while (ADDR_LT((ptr_t)p, plim)) {
    word mark_word = *mark_word_addr++;
    ptr_t *q;

    for (q = p; mark_word != 0; mark_word >>= 4) {
      if (mark_word & 1) {
        PUSH_GRANULE(q);
        PUSH_GRANULE(q + GC_GRANULE_PTRS);
        PUSH_GRANULE(q + 2 * GC_GRANULE_PTRS);
        PUSH_GRANULE(q + 3 * GC_GRANULE_PTRS);
      }
      q += 4 * GC_GRANULE_PTRS;
    }
    p += CPP_WORDSZ * GC_GRANULE_PTRS;
  }
#      undef GC_greatest_plausible_heap_addr
#      undef GC_least_plausible_heap_addr
#      undef GC_mark_stack_top
#      undef GC_mark_stack_limit
#      define GC_mark_stack_limit GC_arrays._mark_stack_limit
#      define GC_mark_stack_top GC_arrays._mark_stack_top
  GC_mark_stack_top = mark_stack_top;
}
#    endif
#  endif
#endif /* !USE_MARK_BYTES && !MARK_BIT_PER_OBJ && !SMALL_CONFIG */

/* Push all objects reachable from marked objects in the given block. */
STATIC void
GC_push_marked(struct hblk *h, const hdr *hhdr)
{
  size_t sz = hhdr->hb_sz;
  ptr_t p;
  size_t bit_no;
  ptr_t plim;
  mse *mark_stack_top;
  mse *mark_stack_limit = GC_mark_stack_limit;

  /* Some quick shortcuts: */
  if ((/* `0 |` */ GC_DS_LENGTH) == hhdr->hb_descr)
    return;
  if (GC_block_empty(hhdr))
    return; /*< nothing marked */

#if !defined(GC_DISABLE_INCREMENTAL)
  GC_n_rescuing_pages++;
#endif
  GC_objects_are_marked = TRUE;
  switch (BYTES_TO_GRANULES(sz)) {
#ifdef USE_PUSH_MARKED_ACCELERATORS
  case 1:
    GC_push_marked1(h, hhdr);
    break;
#  ifndef UNALIGNED_PTRS
  case 2:
    GC_push_marked2(h, hhdr);
    break;
#    if GC_GRANULE_PTRS < 4
  case 4:
    GC_push_marked4(h, hhdr);
    break;
#    endif
#  endif /* !UNALIGNED_PTRS */
#else
  case 1: /*< to suppress "switch statement contains no case" warning */
#endif
  default:
    plim = sz > MAXOBJBYTES ? h->hb_body
                            : CAST_THRU_UINTPTR(ptr_t, (h + 1)->hb_body) - sz;
    mark_stack_top = GC_mark_stack_top;
    for (p = h->hb_body, bit_no = 0; ADDR_GE(plim, p);
         p += sz, bit_no += MARK_BIT_OFFSET(sz)) {
      /* Mark from fields inside the object. */
      if (mark_bit_from_hdr(hhdr, bit_no)) {
        mark_stack_top
            = GC_ms_push_obj_hdr(p, hhdr, mark_stack_top, mark_stack_limit);
      }
    }
    GC_mark_stack_top = mark_stack_top;
  }
}

#ifdef ENABLE_DISCLAIM
/*
 * Unconditionally mark from all objects that have not been reclaimed.
 * This is useful in order to retain pointers reachable from the disclaim
 * notifiers.  To determine whether an object has been reclaimed, we
 * require that any live object has a nonzero as one of the two least
 * significant bits of the first "pointer-sized" word.  On the other hand,
 * the reclaimed object is a member of free lists, and thus contains
 * a pointer-aligned next-pointer as the first "pointer-sized" word.
 */
GC_ATTR_NO_SANITIZE_THREAD
STATIC void
GC_push_unconditionally(struct hblk *h, const hdr *hhdr)
{
  size_t sz = hhdr->hb_sz;
  ptr_t p;
  ptr_t plim;
  mse *mark_stack_top;
  mse *mark_stack_limit = GC_mark_stack_limit;

  if ((/* `0 |` */ GC_DS_LENGTH) == hhdr->hb_descr)
    return;

#  if !defined(GC_DISABLE_INCREMENTAL)
  GC_n_rescuing_pages++;
#  endif
  GC_objects_are_marked = TRUE;
  plim = sz > MAXOBJBYTES ? h->hb_body
                          : CAST_THRU_UINTPTR(ptr_t, (h + 1)->hb_body) - sz;
  mark_stack_top = GC_mark_stack_top;
  for (p = h->hb_body; ADDR_GE(plim, p); p += sz) {
    if ((ADDR(*(ptr_t *)p) & 0x3) != 0) {
      mark_stack_top
          = GC_ms_push_obj_hdr(p, hhdr, mark_stack_top, mark_stack_limit);
    }
  }
  GC_mark_stack_top = mark_stack_top;
}
#endif /* ENABLE_DISCLAIM */

#ifndef GC_DISABLE_INCREMENTAL
/* Test whether any page in the given block is dirty. */
STATIC GC_bool
GC_block_was_dirty(const struct hblk *h, const hdr *hhdr)
{
  size_t sz;
  ptr_t p;

#  ifdef AO_HAVE_load
  /* Atomic access is used to avoid racing with `GC_realloc`. */
  sz = AO_load((volatile AO_t *)&hhdr->hb_sz);
#  else
  sz = hhdr->hb_sz;
#  endif
  if (sz <= MAXOBJBYTES) {
    return GC_page_was_dirty(h);
  }

  for (p = (ptr_t)h; ADDR_LT(p, (ptr_t)h + sz); p += HBLKSIZE) {
    if (GC_page_was_dirty((const struct hblk *)p))
      return TRUE;
  }
  return FALSE;
}
#endif /* GC_DISABLE_INCREMENTAL */

/*
 * Similar to `GC_push_marked`, but skip over unallocated blocks and
 * return address of next plausible block.
 */
STATIC struct hblk *
GC_push_next_marked(struct hblk *h)
{
  const hdr *hhdr = HDR(h);

  if (UNLIKELY(IS_FORWARDING_ADDR_OR_NIL(hhdr) || HBLK_IS_FREE(hhdr))) {
    h = GC_next_block(h, FALSE);
    if (NULL == h)
      return NULL;
    hhdr = GC_find_header(h);
  } else {
#ifdef LINT2
    if (NULL == h)
      ABORT("Bad HDR() definition");
#endif
  }
#ifdef ENABLE_DISCLAIM
  if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0) {
    /*
     * The recovery from a mark stack overflow pushes the marked objects
     * again by this function, but the entries dropped on the overflow
     * might include ones pushed by `GC_push_unconditionally()` for the
     * objects which are not marked (thus not pushed by `GC_push_marked`).
     */
    GC_push_unconditionally(h, hhdr);
  } else
#endif
  /* else */ {
    GC_push_marked(h, hhdr);
  }
  return h + OBJ_SZ_TO_BLOCKS(hhdr->hb_sz);
}

#ifndef GC_DISABLE_INCREMENTAL
/* Identical to `GC_push_next_marked`, but mark only from dirty pages. */
STATIC struct hblk *
GC_push_next_marked_dirty(struct hblk *h)
{
  const hdr *hhdr;

  GC_ASSERT(I_HOLD_LOCK());
  if (!GC_incremental)
    ABORT("Dirty bits not set up");
  for (;; h += OBJ_SZ_TO_BLOCKS(hhdr->hb_sz)) {
    hhdr = HDR(h);
    if (UNLIKELY(IS_FORWARDING_ADDR_OR_NIL(hhdr) || HBLK_IS_FREE(hhdr))) {
      h = GC_next_block(h, FALSE);
      if (NULL == h)
        return NULL;
      hhdr = GC_find_header(h);
    } else {
#  ifdef LINT2
      if (NULL == h)
        ABORT("Bad HDR() definition");
#  endif
    }
    if (GC_block_was_dirty(h, hhdr))
      break;
  }
#  ifdef ENABLE_DISCLAIM
  if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0) {
    GC_push_unconditionally(h, hhdr);
  } else
#  endif
  /* else */ {
    GC_push_marked(h, hhdr);
  }
  return h + OBJ_SZ_TO_BLOCKS(hhdr->hb_sz);
}
#endif /* !GC_DISABLE_INCREMENTAL */

/*
 * Similar to `GC_push_next_marked`, but for uncollectible pages.
 * Needed since we do not clear marks for such pages, even for full
 * collections.
 */
STATIC struct hblk *
GC_push_next_marked_uncollectable(struct hblk *h)
{
  const hdr *hhdr = HDR(h);

  for (;;) {
    if (UNLIKELY(IS_FORWARDING_ADDR_OR_NIL(hhdr) || HBLK_IS_FREE(hhdr))) {
      h = GC_next_block(h, FALSE);
      if (NULL == h)
        return NULL;
      hhdr = GC_find_header(h);
    } else {
#ifdef LINT2
      if (NULL == h)
        ABORT("Bad HDR() definition");
#endif
    }
    if (hhdr->hb_obj_kind == UNCOLLECTABLE) {
      GC_push_marked(h, hhdr);
      break;
    }
#ifdef ENABLE_DISCLAIM
    if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0) {
      GC_push_unconditionally(h, hhdr);
      break;
    }
#endif
    h += OBJ_SZ_TO_BLOCKS(hhdr->hb_sz);
    hhdr = HDR(h);
  }
  return h + OBJ_SZ_TO_BLOCKS(hhdr->hb_sz);
}
