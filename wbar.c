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
 * Support for a client write barrier in the manual VDB mode, and its
 * checker (`GC_WBAR_VERIFY`), which reports pointer stores that no barrier
 * recorded.
 *
 * The checker runs inside `GC_stopped_mark()`, with the world stopped and
 * the allocator lock held, at every read of the dirty bits that follows
 * a complete mark: the first world-stopped mark of a minor collection and
 * the final world-stopped marks of every collection marked in slices.
 * Every marked object has been scanned then.  Right after the dirty bits
 * are read, it scans every marked object of every pointer block that the
 * collection will not rescan, and records each word which points to an
 * unmarked heap object (a candidate).  The root push of the same mark
 * (which comes before the mark can be abandoned) flags the candidates
 * whose target a thread stack or register references.  If the marking
 * completes in the same mark, a candidate whose target is still unmarked
 * is LOST (the sweep would reclaim a referenced object), one whose target
 * is marked but no stack references is MISSED (no thread was between the
 * store and its barrier, so the barrier is missing).  If the mark is
 * abandoned, a candidate whose target no stack references is MISSED.  The
 * others are deferred to the next read: WINDOW if the block of the word
 * is dirty by then (a thread was stopped between its store and its
 * barrier), else MISSED.  The target of a report is marked (thus kept
 * alive) if it is not, so that the run goes on.
 *
 * A report stands only if the next full collection finds its object
 * reachable, and its target was not on a free list: a minor collection
 * keeps the marks of dead objects, a dead object may point to memory
 * freed or reused since, and a live one may point to memory the client
 * freed explicitly.  A report makes the next collection a full one.  The
 * reports that stand and the stale ones are printed after the world is
 * restarted, and then the process exits with status 70 if one stands.
 */

#include "private/gc_pmark.h"

#ifndef NO_MANUAL_VDB

#  if defined(GC_PTHREADS) && !defined(GC_WIN32_THREADS) \
      && defined(THREAD_LOCAL_ALLOC)
#    include "private/pthread_support.h"
#  endif
#  if defined(UNIX_LIKE) || defined(DARWIN) || defined(LINUX)
#    include <unistd.h>
#  endif

GC_INNER GC_bool GC_wbar_verify = FALSE;
GC_INNER word GC_wbar_always_kinds = 0;
GC_INNER GC_bool GC_wbar_need_full = FALSE;

static const char *(GC_CALLBACK *wbar_describe_fn)(const void *, size_t) = 0;

static struct GC_wbar_totals_s tot;

/*
 * Whether a collection is open (its first world-stopped mark has read the
 * dirty bits, and its marking is not complete yet), whether it is a minor
 * one, and the reports of lost and of missed stores that stand, counted
 * from its first world-stopped mark and added to the totals when it
 * completes.
 */
static GC_bool coll_open, coll_minor;
static word coll_lost, coll_missed;

/* The number of reports waiting for the next full collection. */
static size_t n_pending;

/*
 * Exit at once with status 70.  The allocator lock may be held, so
 * a crash handler that forks would wait for it in the at-fork handler.
 */
static void
wbar_exit(void)
{
#  if defined(UNIX_LIKE) || defined(DARWIN) || defined(LINUX)
  _exit(70);
#  else
  ABORT("GC_WBAR_VERIFY: pointer stores without a write barrier");
#  endif
}

#  ifndef DONT_USE_ATEXIT
/*
 * At exit, let a full collection confirm the reports still waiting, and
 * exit with status 70 if a report stands: the thread whose collection
 * found it may still be printing when another thread calls `exit()`.
 */
static void
wbar_atexit(void)
{
  if (n_pending > 0)
    GC_gcollect();
  /* Without the allocator lock, which the printing thread holds. */
  if (tot.lost + tot.missed != 0)
    wbar_exit();
}
#  endif

GC_INNER void
GC_wbar_init(void)
{
  const char *s = GETENV("GC_WBAR_VERIFY");

  if (s != NULL && *s != '\0' && *s != '0') {
    GC_wbar_verify = TRUE;
#  ifndef DONT_USE_ATEXIT
    atexit(wbar_atexit);
#  endif
  }
}

GC_API volatile GC_word *GC_CALL
GC_get_dirty_bits(unsigned *log_block_size, unsigned *log_entries)
{
  if (log_block_size != NULL)
    *log_block_size = LOG_HBLKSIZE;
  if (log_entries != NULL)
    *log_entries = LOG_PHT_ENTRIES;
  return (volatile GC_word *)GC_dirty_pages;
}

GC_API void GC_CALL
GC_set_kind_always_rescanned(int kind, int value)
{
  if (kind < 0 || kind >= (int)(sizeof(word) * 8))
    ABORT("GC_set_kind_always_rescanned: kind out of range");
  LOCK();
  if (value) {
    GC_wbar_always_kinds |= (word)1 << kind;
  } else {
    GC_wbar_always_kinds &= ~((word)1 << kind);
  }
  UNLOCK();
}

GC_INNER void
GC_wbar_dirty_blocks(const void *p, size_t nblocks)
{
  ptr_t h = (ptr_t)HBLKPTR(p);
  size_t i;

  for (i = 0; i < nblocks; i++)
    GC_wbar_dirty(h + i * HBLKSIZE);
}

GC_INNER GC_bool
GC_wbar_large_by_block(const hdr *hhdr)
{
  if (!GC_manual_vdb || hhdr->hb_sz <= MAXOBJBYTES
      || (hhdr->hb_descr & GC_DS_TAGS) != GC_DS_LENGTH
      || GC_wbar_always_rescanned(hhdr))
    return FALSE;
#  ifdef ENABLE_DISCLAIM
  if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0)
    return FALSE;
#  endif
  return TRUE;
}

GC_INNER void
GC_wbar_dirty_object(const void *p)
{
  hdr *hhdr = HDR(p);
  ptr_t h;
  size_t i, n;

  if (NULL == hhdr || !GC_manual_vdb)
    return;
  if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
    /* An interior block of a large object. */
    ptr_t base = (ptr_t)GC_base(MAKE_CPTR(ADDR(p)));

    if (NULL == base)
      return;
    hhdr = HDR(base);
  }
  if (HBLK_IS_FREE(hhdr) || hhdr->hb_sz <= MAXOBJBYTES
      || !GC_wbar_large_by_block(hhdr))
    return;
  h = (ptr_t)hhdr->hb_block;
  n = OBJ_SZ_TO_BLOCKS(hhdr->hb_sz);
  for (i = 0; i < n; i++) {
    size_t index = PHT_HASH(h + i * HBLKSIZE);

    if (!get_pht_entry_from_index(GC_dirty_pages, index))
      set_pht_entry_from_index_concurrent(GC_dirty_pages, index);
  }
}

GC_API void GC_CALL
GC_set_wbar_verify_describe(const char *(GC_CALLBACK *fn)(const void *,
                                                          size_t))
{
  LOCK();
  wbar_describe_fn = fn;
  UNLOCK();
}

void GC_CALL
GC_get_wbar_verify_totals(struct GC_wbar_totals_s *pt)
{
  READER_LOCK();
  *pt = tot;
  pt->pending = (word)n_pending;
  READER_UNLOCK();
}

/*
 * The tables of the checker, allocated from the scratch space (which the
 * collector does not scan) at the first collection that needs them.
 */
#  define WBAR_MAX_CAND 65536
#  define WBAR_MAX_DEFERRED 4096
#  define WBAR_MAX_REPORTS 64
#  define WBAR_MAX_PENDING 4096
#  define WBAR_HASH_SZ (2 * WBAR_MAX_CAND)

/* The kinds of reports, which index `what_names`. */
#  define WBAR_LOST 0
#  define WBAR_MISSED 1
#  define WBAR_MISSED_LATER 2

static const char *const what_names[] = { "LOST", "MISSED", "MISSED(next)" };

struct wbar_cand {
  word obj;    /*< the marked object holding the pointer */
  word obj_sz; /*< its size */
  word slot;   /*< the address of the word */
  word value;  /*< the word */
  word target; /*< the unmarked object it points into */
  word target_sz;
  int obj_kind, target_kind;
  /*
   * Whether the target was on a free list when the candidate was found:
   * the word then points to freed memory, which no barrier is about.
   */
  int target_free;
  /* The first words of the object, to tell its type. */
  word head[4];
};

struct wbar_report {
  struct wbar_cand c;
  int what;
  GC_bool stale; /*< discarded by the collection that checked it */
  word gc_no;    /*< the collection that found it */
  word check_no; /*< the collection that confirmed or discarded it */
};

static struct wbar_cand *cands;
static size_t n_cands;
/* The candidates the table had no room for, since the latest check. */
static word n_dropped;
/* Open-addressed set of the targets of the candidates: index + 1 or 0. */
static size_t *cand_hash;
static unsigned char *cand_stack_held;
static struct wbar_cand *deferred;
static size_t n_deferred;
static struct wbar_report *reports;
static size_t n_reports;
static struct wbar_report *pending;
static GC_bool verify_tables_failed = FALSE;

static GC_bool
alloc_verify_tables(void)
{
  if (cands != NULL)
    return TRUE;
  if (verify_tables_failed)
    return FALSE;
  cands = (struct wbar_cand *)GC_scratch_alloc(WBAR_MAX_CAND
                                               * sizeof(struct wbar_cand));
  cand_hash = (size_t *)GC_scratch_alloc(WBAR_HASH_SZ * sizeof(size_t));
  cand_stack_held = (unsigned char *)GC_scratch_alloc(WBAR_MAX_CAND);
  deferred = (struct wbar_cand *)GC_scratch_alloc(WBAR_MAX_DEFERRED
                                                  * sizeof(struct wbar_cand));
  reports = (struct wbar_report *)GC_scratch_alloc(
      WBAR_MAX_REPORTS * sizeof(struct wbar_report));
  pending = (struct wbar_report *)GC_scratch_alloc(
      WBAR_MAX_PENDING * sizeof(struct wbar_report));
  if (NULL == cands || NULL == cand_hash || NULL == cand_stack_held
      || NULL == deferred || NULL == reports || NULL == pending) {
    cands = NULL;
    verify_tables_failed = TRUE;
    return FALSE;
  }
  return TRUE;
}

static size_t
hash_index(word target)
{
  word h = target >> 4;

  h ^= h >> 17;
  h *= (word)0x9E3779B97F4A7C15ULL;
  return (size_t)(h >> 7) % WBAR_HASH_SZ;
}

/* Return the index of the first candidate with `target`, or -1. */
static GC_signed_word
find_target(word target)
{
  size_t i = hash_index(target);

  for (;;) {
    size_t e = cand_hash[i];

    if (0 == e)
      return -1;
    if (cands[e - 1].target == target)
      return (GC_signed_word)(e - 1);
    i = (i + 1) % WBAR_HASH_SZ;
  }
}

static void
add_target(size_t cand_index)
{
  word target = cands[cand_index].target;
  size_t i = hash_index(target);

  for (;;) {
    size_t e = cand_hash[i];

    if (0 == e) {
      cand_hash[i] = cand_index + 1;
      return;
    }
    if (cands[e - 1].target == target)
      return; /*< the first candidate represents the target */
    i = (i + 1) % WBAR_HASH_SZ;
  }
}

static GC_bool
obj_marked(word base)
{
  return GC_is_marked((const void *)base) != 0;
}

/* Keep a report for the printing after the world is restarted. */
static void
print_later(const struct wbar_report *r)
{
  if (n_reports < WBAR_MAX_REPORTS)
    reports[n_reports++] = *r;
}

static void
report_stands(const struct wbar_report *r)
{
  if (WBAR_LOST == r->what) {
    coll_lost++;
  } else {
    coll_missed++;
  }
  print_later(r);
}

static void
report_stale(struct wbar_report *r)
{
  r->stale = TRUE;
  tot.stale++;
  print_later(r);
}

/*
 * A report waits for the next full collection.  If too many wait, it
 * stands at once, unless its target was on a free list (see
 * `check_pending()`): then it is stale at once.
 */
static void
add_report(const struct wbar_cand *c, int what)
{
  struct wbar_report r;

  r.c = *c;
  r.what = what;
  r.stale = FALSE;
  r.gc_no = GC_gc_no + 1;
  r.check_no = GC_gc_no + 1;
  if (n_pending < WBAR_MAX_PENDING) {
    pending[n_pending++] = r;
  } else if (c->target_free) {
    report_stale(&r);
  } else {
    report_stands(&r);
  }
  GC_wbar_need_full = TRUE;
}

/*
 * Mark the target of candidate `c` (if not marked) and push it, so that
 * the run goes on until the report is confirmed.  Return whether it was
 * pushed.
 */
static GC_bool
keep_alive(const struct wbar_cand *c)
{
  hdr *thdr;

  if (c->target_free || obj_marked(c->target))
    return FALSE;
  thdr = HDR((ptr_t)c->target);
  if (NULL == thdr || IS_FORWARDING_ADDR_OR_NIL(thdr) || HBLK_IS_FREE(thdr))
    return FALSE;
  GC_mark_stack_top
      = GC_ms_push_contents_hdr((ptr_t)c->target, thdr, GC_mark_stack_top,
                                GC_mark_stack_limit, NULL, FALSE);
  return TRUE;
}

/*
 * Confirm or discard the pending reports at the end of the marking of a
 * full collection: a report stands if its object is marked, i.e.
 * reachable (and still allocated with the same size), and its target was
 * not on a free list when it was found (a pointer to memory freed before
 * the collection that found it is no missed store of that interval).
 */
static void
check_pending(void)
{
  size_t i;

  for (i = 0; i < n_pending; i++) {
    struct wbar_report *r = &pending[i];
    const hdr *hhdr = HDR((ptr_t)r->c.obj);

    r->check_no = GC_gc_no + 1;
    if (!r->c.target_free && hhdr != NULL && !IS_FORWARDING_ADDR_OR_NIL(hhdr)
        && !HBLK_IS_FREE(hhdr) && hhdr->hb_sz == r->c.obj_sz
        && obj_marked(r->c.obj)) {
      report_stands(r);
    } else {
      report_stale(r);
    }
  }
  n_pending = 0;
  GC_wbar_need_full = FALSE;
}

#  if defined(GC_PTHREADS) && !defined(GC_WIN32_THREADS) \
      && defined(THREAD_LOCAL_ALLOC)
/* Is `p` inside the thread-local free-list heads of some thread? */
static GC_bool
in_tlfs(ptr_t p)
{
  int i;
  GC_thread t;

  for (i = 0; i < THREAD_TABLE_SZ; ++i) {
    for (t = GC_threads[i]; t != NULL; t = t->tm.next) {
      if (ADDR_GE(p, (ptr_t)&t->tlfs)
          && ADDR_LT(p, (ptr_t)&t->tlfs + sizeof(t->tlfs)))
        return TRUE;
    }
  }
  return FALSE;
}

/* Is `obj` on a thread-local free list of `kind` and size `lg`? */
static GC_bool
on_tl_free_list(ptr_t obj, int kind, size_t lg)
{
  int i;
  GC_thread t;

  if (kind >= THREAD_FREELISTS_KINDS || lg >= GC_TINY_FREELISTS)
    return FALSE;
  for (i = 0; i < THREAD_TABLE_SZ; ++i) {
    for (t = GC_threads[i]; t != NULL; t = t->tm.next) {
      ptr_t q = (ptr_t)t->tlfs._freelists[kind][lg];

      for (; ADDR(q) > HBLKSIZE; q = (ptr_t)obj_link(q)) {
        if (q == obj)
          return TRUE;
      }
    }
  }
  return FALSE;
}
#  else
#    define in_tlfs(p) FALSE
#    define on_tl_free_list(obj, kind, lg) FALSE
#  endif

/*
 * Is the small object `obj` on a free list?  The objects of a thread-local
 * free list are marked, and so is an object freed explicitly while marked;
 * their links point to unmarked free objects, which need no barrier.
 */
static GC_bool
obj_on_free_list(ptr_t obj, const hdr *hhdr)
{
  size_t lg;
  int kind = hhdr->hb_obj_kind;

  if (hhdr->hb_sz > MAXOBJBYTES)
    return FALSE;
  lg = BYTES_TO_GRANULES(hhdr->hb_sz);
  if (GC_obj_kinds[kind].ok_freelist != NULL) {
    ptr_t q;

    for (q = (ptr_t)GC_obj_kinds[kind].ok_freelist[lg]; q != NULL;
         q = (ptr_t)obj_link(q)) {
      if (q == obj)
        return TRUE;
    }
  }
  return on_tl_free_list(obj, kind, lg);
}

/* Check one word of the marked object `obj` of block header `hhdr`. */
GC_INLINE void
check_word(ptr_t obj, const hdr *hhdr, ptr_t slot)
{
  ptr_t q = *(ptr_t *)slot;
  ptr_t base;
  const hdr *thdr;
  struct wbar_cand *c;

  if (ADDR_GE((ptr_t)GC_least_plausible_heap_addr, q)
      || ADDR_GE(q, (ptr_t)GC_greatest_plausible_heap_addr))
    return;
  if (NULL == HDR(q))
    return;
  base = (ptr_t)GC_base(q);
  if (NULL == base)
    return;
  if (obj_marked(ADDR(base)))
    return;
  /* The heads of the thread-local free lists are marked directly. */
  if (in_tlfs(slot) || obj_on_free_list(obj, hhdr))
    return;
  thdr = HDR(base);
  if (n_cands >= WBAR_MAX_CAND) {
    n_dropped++;
    return;
  }
  c = &cands[n_cands];
  c->obj = ADDR(obj);
  c->obj_sz = hhdr->hb_sz;
  c->slot = ADDR(slot);
  c->value = ADDR(q);
  c->target = ADDR(base);
  c->target_sz = thdr->hb_sz;
  c->obj_kind = hhdr->hb_obj_kind;
  c->target_kind = thdr->hb_obj_kind;
  c->target_free = (int)obj_on_free_list(base, thdr);
  {
    size_t i;

    for (i = 0; i < 4; i++)
      c->head[i] = i * sizeof(word) < hhdr->hb_sz ? ((word *)obj)[i] : 0;
  }
  cand_stack_held[n_cands] = 0;
  add_target(n_cands);
  n_cands++;
}

/* Check every pointer word of the marked object `p` per its descriptor. */
static void
check_obj(ptr_t p, const hdr *hhdr)
{
  word descr = hhdr->hb_descr;
  size_t sz = hhdr->hb_sz;
  ptr_t q, lim;

  switch (descr & GC_DS_TAGS) {
  case GC_DS_LENGTH:
    lim = p + (descr < sz ? (size_t)descr : sz) - sizeof(ptr_t);
    for (q = p; ADDR_GE(lim, q); q += sizeof(ptr_t))
      check_word(p, hhdr, q);
    break;
  case GC_DS_BITMAP:
    descr &= ~(word)GC_DS_TAGS;
    for (q = p; descr != 0 && ADDR_LT(q, p + sz); q += sizeof(ptr_t)) {
      if ((descr & SIGNB) != 0)
        check_word(p, hhdr, q);
      descr <<= 1;
    }
    break;
  default:
    /* A mark procedure or a per-object descriptor: not checked. */
    break;
  }
}

/*
 * Check the clean blocks of the marked large object `h`, of which a pass
 * over the dirty blocks pushes only the dirty ones (see
 * `GC_wbar_large_by_block()`).
 */
static void
check_large_by_block(struct hblk *h, const hdr *hhdr)
{
  word descr = hhdr->hb_descr;
  ptr_t start = h->hb_body;
  ptr_t lim = start + (descr < hhdr->hb_sz ? (size_t)descr : hhdr->hb_sz);
  ptr_t b;

  if (!mark_bit_from_hdr(hhdr, 0))
    return;
  for (b = start; ADDR_LT(b, lim); b += HBLKSIZE) {
    ptr_t q;
    ptr_t blim = ADDR_LT(b + HBLKSIZE, lim) ? b + HBLKSIZE : lim;

    if (GC_page_was_dirty((struct hblk *)b))
      continue;
    for (q = b; ADDR_LT(q, blim - sizeof(ptr_t) + 1); q += sizeof(ptr_t))
      check_word(start, hhdr, q);
  }
}

static void
check_block(hdr *hhdr, void *cd)
{
  struct hblk *h = hhdr->hb_block;
  size_t sz = hhdr->hb_sz;

  UNUSED_ARG(cd);
  if ((/* `0 |` */ GC_DS_LENGTH) == hhdr->hb_descr || GC_block_empty(hhdr))
    return;
  if (sz > MAXOBJBYTES && GC_wbar_large_by_block(hhdr)) {
    check_large_by_block(h, hhdr);
    return;
  }
  /* The same predicate as `GC_push_next_marked_dirty()`. */
  if (GC_block_was_dirty(h, hhdr))
    return;
#  ifdef ENABLE_DISCLAIM
  if ((hhdr->hb_flags & MARK_UNCONDITIONALLY) != 0)
    return;
#  endif
  if (sz > MAXOBJBYTES) {
    if (mark_bit_from_hdr(hhdr, 0))
      check_obj(h->hb_body, hhdr);
  } else {
    ptr_t p;
    ptr_t plim = CAST_THRU_UINTPTR(ptr_t, (h + 1)->hb_body) - sz;
    size_t bit_no;

    for (p = h->hb_body, bit_no = 0; ADDR_GE(plim, p);
         p += sz, bit_no += MARK_BIT_OFFSET(sz)) {
      if (mark_bit_from_hdr(hhdr, bit_no))
        check_obj(p, hhdr);
    }
  }
}

/* Resolve the candidates deferred by the previous collection. */
static void
resolve_deferred(void)
{
  size_t i;

  for (i = 0; i < n_deferred; i++) {
    const struct wbar_cand *c = &deferred[i];
    const hdr *hhdr = HDR((ptr_t)c->obj);
    const struct hblk *h;

    /* The object may have been reclaimed by a full collection since. */
    if (NULL == hhdr || IS_FORWARDING_ADDR_OR_NIL(hhdr) || HBLK_IS_FREE(hhdr)
        || hhdr->hb_sz != c->obj_sz)
      continue;
    h = HBLKPTR((ptr_t)c->obj);
    if (hhdr->hb_sz > MAXOBJBYTES)
      h = hhdr->hb_block;
    if (hhdr->hb_sz > MAXOBJBYTES && GC_wbar_large_by_block(hhdr)
            /* Of a large object rescanned by block, the block of the word. */
            ? GC_page_was_dirty(HBLKPTR((ptr_t)c->slot))
            : GC_block_was_dirty(h, hhdr)) {
      tot.window++;
    } else {
      add_report(c, WBAR_MISSED_LATER);
    }
  }
  n_deferred = 0;
}

GC_INNER void
GC_wbar_after_read_dirty(GC_bool marks_complete)
{
  GC_ASSERT(I_HOLD_LOCK());
  if (!GC_manual_vdb)
    return;
  if (!coll_open) {
    /* The first world-stopped mark of a collection. */
    coll_open = TRUE;
    coll_minor = marks_complete;
    coll_lost = 0;
    coll_missed = 0;
  }
  if (!alloc_verify_tables()) {
    WARN("GC_WBAR_VERIFY: no memory for the tables, check skipped\n", 0);
    return;
  }
  resolve_deferred();
  n_cands = 0;
  n_dropped = 0;
  BZERO(cand_hash, WBAR_HASH_SZ * sizeof(size_t));
  if (marks_complete) {
    GC_apply_to_all_hdrs(check_block, NULL);
    tot.checks++;
  }
}

#  ifdef THREADS
GC_INNER void
GC_wbar_note_stack_ref(ptr_t p)
{
  ptr_t base;
  GC_signed_word i;

  if (0 == n_cands)
    return;
  base = (ptr_t)GC_base(p);
  if (NULL == base)
    return;
  i = find_target(ADDR(base));
  if (i >= 0)
    cand_stack_held[i] = 1;
}
#  endif

GC_INNER void
GC_wbar_after_mark(GC_bool completed)
{
  size_t i;
  GC_bool pushed = FALSE;

  GC_ASSERT(I_HOLD_LOCK());
  if (!GC_manual_vdb || NULL == cands)
    return;
  for (i = 0; i < n_cands; i++) {
    const struct wbar_cand *c = &cands[i];
    GC_signed_word first = find_target(c->target);

    if (completed && !obj_marked(c->target)) {
      add_report(c, WBAR_LOST);
    } else if (first >= 0 && cand_stack_held[first]) {
      if (n_deferred < WBAR_MAX_DEFERRED)
        deferred[n_deferred++] = *c;
    } else {
      add_report(c, WBAR_MISSED);
    }
  }
  /* Keep the targets of the reports alive, after classifying them all. */
  for (i = 0; i < n_cands; i++) {
    const struct wbar_cand *c = &cands[i];
    GC_signed_word first = find_target(c->target);

    if ((completed || !(first >= 0 && cand_stack_held[first]))
        && keep_alive(c))
      pushed = TRUE;
  }
  n_cands = 0;
  if (completed) {
    if (pushed) {
      /* Mark from the pushed targets before the sweep. */
      GC_mark_state = MS_ROOTS_PUSHED;
      while (!GC_mark_some(NULL)) {
        /* Empty. */
      }
    }
    /* Every marked object was marked by this full mark. */
    if (!coll_minor)
      check_pending();
  }
}

GC_INNER void
GC_wbar_after_start_world(GC_bool completed)
{
  size_t i;
  word found;

  GC_ASSERT(I_HOLD_LOCK());
  if (!GC_manual_vdb)
    return;
  if (n_dropped != 0) {
    /* The check is incomplete. */
    GC_log_printf("GC_WBAR_VERIFY: %lu candidates dropped at collection"
                  " #%lu, the table is full\n",
                  (unsigned long)n_dropped,
                  (unsigned long)(completed ? GC_gc_no : GC_gc_no + 1));
  }
  if (!completed)
    return;
  coll_open = FALSE;
  found = coll_lost + coll_missed;
  tot.lost += coll_lost;
  tot.missed += coll_missed;
  for (i = 0; i < n_reports; i++) {
    const struct wbar_report *r = &reports[i];
    const char *desc = NULL;

    if (wbar_describe_fn != 0)
      desc = wbar_describe_fn((const void *)r->c.obj, (size_t)r->c.obj_sz);
    GC_log_printf(
        "WBVERIFY %s%s gc=%lu checked_gc=%lu obj=%p sz=%lu kind=%d block=%p"
        " ofs=%lu value=%p target=%p target_sz=%lu target_kind=%d"
        " target_free=%d class=%s head=%p,%p,%p,%p\n",
        r->stale ? "STALE " : "", what_names[r->what], (unsigned long)r->gc_no,
        (unsigned long)r->check_no, (void *)r->c.obj,
        (unsigned long)r->c.obj_sz, r->c.obj_kind, (void *)HBLKPTR(r->c.obj),
        (unsigned long)(r->c.slot - r->c.obj), (void *)r->c.value,
        (void *)r->c.target, (unsigned long)r->c.target_sz, r->c.target_kind,
        r->c.target_free, desc != NULL ? desc : "-", (void *)r->c.head[0],
        (void *)r->c.head[1], (void *)r->c.head[2], (void *)r->c.head[3]);
  }
  n_reports = 0;
  if (found != 0) {
    GC_log_printf("GC_WBAR_VERIFY: pointer stores without a write barrier,"
                  " exiting with status 70\n");
    wbar_exit();
  }
}

#else /* NO_MANUAL_VDB */

GC_API volatile GC_word *GC_CALL
GC_get_dirty_bits(unsigned *log_block_size, unsigned *log_entries)
{
  UNUSED_ARG(log_block_size);
  UNUSED_ARG(log_entries);
  return NULL;
}

GC_API void GC_CALL
GC_set_kind_always_rescanned(int kind, int value)
{
  UNUSED_ARG(kind);
  UNUSED_ARG(value);
}

GC_API void GC_CALL
GC_set_wbar_verify_describe(const char *(GC_CALLBACK *fn)(const void *,
                                                          size_t))
{
  UNUSED_ARG(fn);
}

#endif /* NO_MANUAL_VDB */
