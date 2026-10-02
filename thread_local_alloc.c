/*
 * Copyright (c) 2000-2005 by Hewlett-Packard Company.  All rights reserved.
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

#include "private/gc_priv.h"

#if defined(THREAD_LOCAL_ALLOC)

#  if !defined(THREADS) && !defined(CPPCHECK)
#    error Invalid config - THREAD_LOCAL_ALLOC requires GC_THREADS
#  endif

#  include "private/thread_local_alloc.h"

#  if defined(USE_COMPILER_TLS)
__thread GC_ATTR_TLS_FAST
#  elif defined(USE_WIN32_COMPILER_TLS)
__declspec(thread) GC_ATTR_TLS_FAST
#  endif
    GC_key_t GC_thread_key;

#  if !defined(USE_COMPILER_TLS) && !defined(USE_WIN32_COMPILER_TLS)
static GC_bool keys_initialized;
#  endif

#  ifndef GC_NO_DEINIT
GC_INNER void
GC_reset_thread_local_initialization(void)
{
#    if !defined(USE_COMPILER_TLS) && !defined(USE_WIN32_COMPILER_TLS)
  keys_initialized = FALSE;
#    endif
  /* TODO: Dispose resources associated with `GC_thread_key`. */
}
#  endif

/* Initialize all the free lists of a thread-local storage structure. */
static void
init_freelists(GC_tlfs p)
{
  /*
   * A small counter makes the thread allocate the first objects of each
   * size globally (a warm-up), while `NULL` (an empty thread-local free
   * list) makes it refill the free list on its first allocation.
   */
  void *fl_init = GC_no_thread_local_warmup ? NULL : NUMERIC_TO_VPTR(1);
  int kind, j;

  for (j = 0; j < GC_TINY_FREELISTS; ++j) {
    for (kind = 0; kind < THREAD_FREELISTS_KINDS; ++kind) {
      p->_freelists[kind][j] = fl_init;
    }
#  ifdef THREAD_GCJ_FREELISTS
    p->gcj_freelists[j] = fl_init;
#  endif
  }
  /*
   * The zero-sized free list is handled like the regular free list, to
   * ensure that the explicit deallocation works.  However, an allocation
   * of a `gcj` object with the zero size is always an error.
   */
#  ifdef THREAD_GCJ_FREELISTS
  p->gcj_freelists[0] = MAKE_CPTR(ERROR_FL);
#  endif
}

/*
 * Return a single nonempty free list `fl` to the global one pointed to
 * by `gfl`.
 */
static void
return_single_freelist(void *fl, void **gfl)
{
  if (NULL == *gfl) {
    *gfl = fl;
  } else {
    void *q = fl;
    void **q_ptr;

    GC_ASSERT(GC_size(fl) == GC_size(*gfl));
    /* Concatenate. */
    do {
      q_ptr = &obj_link(q);
      q = *q_ptr;
    } while (ADDR(q) >= HBLKSIZE);
    GC_ASSERT(NULL == q);
    *q_ptr = *gfl;
    *gfl = fl;
  }
}

/*
 * Recover the contents of the free-list array `fl` into the global one
 * `gfl`.
 */
static void
return_freelists(void **fl, void **gfl)
{
  int i;

  for (i = 1; i < GC_TINY_FREELISTS; ++i) {
    if (ADDR(fl[i]) >= HBLKSIZE) {
      return_single_freelist(fl[i], &gfl[i]);
    }
    /*
     * Clear `fl[i]`, since the thread structure may hang around.
     * Do it in a way that is likely to trap if we access it.
     */
    fl[i] = (ptr_t)NUMERIC_TO_VPTR(HBLKSIZE);
  }
  /* The 0 granule free list really contains 1 granule objects. */
  if (ADDR(fl[0]) >= HBLKSIZE
#  ifdef THREAD_GCJ_FREELISTS
      && ADDR(fl[0]) != ERROR_FL
#  endif
  ) {
    return_single_freelist(fl[0], &gfl[1]);
  }
}

#  ifdef HAS_WIN32_THREADS_DISCOVERY
static void
return_freelists_async(void **fl, void **gfl, GC_bool is_async)
{
  if (is_async) {
    int i;

    /* TODO: For now these objects will be collected during the next GC. */
    for (i = 1; i < GC_TINY_FREELISTS; ++i) {
      /* Clear `fl[i]`, in a way that is likely to trap if we access it. */
      fl[i] = (ptr_t)NUMERIC_TO_VPTR(HBLKSIZE);
    }
  } else {
    return_freelists(fl, gfl);
  }
}
#  else
#    define return_freelists_async(fl, gfl, a) return_freelists(fl, gfl)
#  endif

#  ifdef USE_PTHREAD_SPECIFIC
/*
 * Re-set the TLS value on thread cleanup to allow thread-local allocations
 * to happen in the TLS destructors.  `GC_unregister_my_thread()` (and
 * similar routines) will finally set the `GC_thread_key` to `NULL`
 * preventing this destructor from being called repeatedly.
 */
static void
reset_thread_key(void *v)
{
  pthread_setspecific(GC_thread_key, v);
}
#  else
#    define reset_thread_key 0
#  endif

GC_INNER void
GC_init_thread_local(GC_tlfs p)
{
#  if !defined(USE_COMPILER_TLS) && !defined(USE_WIN32_COMPILER_TLS)
  GC_ASSERT(I_HOLD_LOCK());
  if (UNLIKELY(!keys_initialized)) {
#    ifdef USE_CUSTOM_SPECIFIC
    /* Ensure proper alignment of a "pushed" GC symbol. */
    ASSERT_ALIGNMENT(&GC_thread_key);
#    endif
    if (GC_key_create(&GC_thread_key, reset_thread_key) != 0)
      ABORT("Failed to create key for local allocator");
    keys_initialized = TRUE;
  }
#  endif
  init_freelists(p);
#  if !defined(USE_COMPILER_TLS) && !defined(USE_WIN32_COMPILER_TLS)
  if (GC_setspecific(GC_thread_key, p) != 0)
    ABORT("Failed to set thread specific allocation pointers");
#  else
  GC_thread_key = p;
#  endif
}

GC_INNER void
#  ifdef HAS_WIN32_THREADS_DISCOVERY
GC_destroy_thread_local_async(GC_tlfs p, GC_bool is_async)
#  else
GC_destroy_thread_local(GC_tlfs p)
#  endif
{
  int kind;

#  ifdef HAS_WIN32_THREADS_DISCOVERY
  GC_ASSERT(is_async || I_HOLD_LOCK());
#  else
  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_getspecific(GC_thread_key) == p);
  /* We do this from the thread itself. */
#  endif
  GC_STATIC_ASSERT(THREAD_FREELISTS_KINDS <= MAXOBJKINDS);
  for (kind = 0; kind < THREAD_FREELISTS_KINDS; ++kind) {
    if (kind == (int)GC_n_kinds) {
      /* The kind is not created. */
      break;
    }
    return_freelists_async(p->_freelists[kind], GC_obj_kinds[kind].ok_freelist,
                           is_async);
  }
#  ifdef THREAD_GCJ_FREELISTS
  return_freelists_async(p->gcj_freelists, (void **)GC_gcjobjfreelist,
                         is_async);
#  endif
}

STATIC void *
GC_get_tlfs(void)
{
#  if defined(USE_PTHREAD_SPECIFIC) || defined(USE_WIN32_SPECIFIC)
  if (UNLIKELY(!keys_initialized))
    return NULL;

  return GC_getspecific(GC_thread_key);
#  else
  GC_key_t k = GC_thread_key;

  if (UNLIKELY(0 == k)) {
    /*
     * We have not yet run `GC_init_parallel()`.  That means we also
     * are not locking, so `GC_malloc_kind_global()` is fairly cheap.
     */
    return NULL;
  }
  return GC_getspecific(k);
#  endif
}

/*
 * The slow path of `malloc_kind_tl()`: the size is too big for the
 * thread-local free lists, or the free list is empty or holds a counter,
 * or the free-list entry should be dirtied manually.  If
 * `GC_NO_TL_MALLOC_FAST_PATH` macro is defined, then every allocation takes
 * this path.  `tiny_fl` is the thread-local free-list array of `kind`.  Not
 * inlined, so that the fast path does not need a stack frame to preserve its
 * arguments across the calls made here.
 */
STATIC GC_ATTR_NOINLINE void *
GC_malloc_kind_tl_slow(size_t lb, int kind, void **tiny_fl)
{
  size_t lg = ALLOC_REQUEST_GRANS(lb);
  void *result;

  GC_FAST_MALLOC_GRANS(
      result, lg, tiny_fl, DIRECT_GRANULES, kind,
      GC_malloc_kind_global(lb, kind),
      (void)(kind == PTRFREE ? NULL : (obj_link(result) = NULL)));
#  ifdef LOG_ALLOCS
  GC_log_printf("GC_malloc_kind(%lu, %d) returned %p, recent GC #%lu\n",
                (unsigned long)lb, kind, result, (unsigned long)GC_gc_no);
#  endif
  return result;
}

/*
 * The thread-local allocation of an object of the given `kind`.  Inlined
 * into `GC_malloc_kind()`, `GC_malloc()` and `GC_malloc_atomic()`, thus
 * the checks of `kind` are done at compile time in the latter two.  If
 * `GC_NO_TL_MALLOC_FAST_PATH` macro is defined, then only
 * `GC_malloc_kind()` uses it, and it always calls the slow path.
 */
GC_INLINE GC_ATTR_ALWAYS_INLINE void *
malloc_kind_tl(size_t lb, int kind)
{
  void *tsd;
  void **tiny_fl;

#  if MAXOBJKINDS > THREAD_FREELISTS_KINDS
  if (UNLIKELY(kind >= THREAD_FREELISTS_KINDS))
    return GC_malloc_kind_global(lb, kind);
#  endif
  tsd = GC_get_tlfs();
  if (UNLIKELY(NULL == tsd))
    return GC_malloc_kind_global(lb, kind);

  GC_ASSERT(GC_is_initialized);
  GC_ASSERT(GC_is_thread_tsd_valid(tsd));
  tiny_fl = ((GC_tlfs)tsd)->_freelists[kind];
#  ifndef GC_NO_TL_MALLOC_FAST_PATH
  /*
   * The case of a nonempty free list of `GC_FAST_MALLOC_GRANS()` follows.
   * The size check ensures that `lg` is less than `GC_TINY_FREELISTS`
   * without the saturated addition of `ALLOC_REQUEST_GRANS()`.  (The
   * biggest size which fits the tiny free lists only if `EXTRA_BYTES` is
   * zero is left to the slow path, which handles it in the same way.)
   */
  if (LIKELY(lb
             <= GRANULES_TO_BYTES(GC_TINY_FREELISTS - 1) - MAX_EXTRA_BYTES)) {
    size_t lg = BYTES_TO_GRANULES(lb + (GC_GRANULE_BYTES - 1) + EXTRA_BYTES);
    void **my_fl = tiny_fl + lg;
    void *result = *my_fl;

    GC_ASSERT(lg == ALLOC_REQUEST_GRANS(lb));
    /*
     * The free-list entry is dirtied (by `GC_end_stubborn_change()` in
     * `GC_FAST_MALLOC_GRANS()`) only in the manual VDB mode, which is
     * left to the slow path.  Thus there is no call here.
     */
    if (LIKELY(ADDR(result) > DIRECT_GRANULES + GC_TINY_FREELISTS + 1)
        && (PTRFREE == kind || LIKELY(!GC_manual_vdb))) {
      void *next = *(void **)result;

      GC_FAST_M_AO_STORE(my_fl, next);
      if (kind != PTRFREE)
        obj_link(result) = NULL;
      GC_PREFETCH_FOR_WRITE(next);
      if (kind != PTRFREE)
        GC_reachable_here(next);
      GC_ASSERT(GC_size(result) >= GRANULES_TO_BYTES(lg));
      GC_ASSERT(PTRFREE == kind || NULL == ((void **)result)[1]);
#    ifdef LOG_ALLOCS
      GC_log_printf("GC_malloc_kind(%lu, %d) returned %p, recent GC #%lu\n",
                    (unsigned long)lb, kind, result, (unsigned long)GC_gc_no);
#    endif
      return result;
    }
  }
#  endif
  return GC_malloc_kind_tl_slow(lb, kind, tiny_fl);
}

GC_API GC_ATTR_MALLOC void *GC_CALL
GC_malloc_kind(size_t lb, int kind)
{
  return malloc_kind_tl(lb, kind);
}

#  ifndef GC_NO_TL_MALLOC_FAST_PATH
GC_API GC_ATTR_MALLOC void *GC_CALL
GC_malloc_atomic(size_t lb)
{
  /* Allocate `lb` bytes of atomic (pointer-free) data. */
  return malloc_kind_tl(lb, PTRFREE);
}

GC_API GC_ATTR_MALLOC void *GC_CALL
GC_malloc(size_t lb)
{
  /* Allocate `lb` bytes of composite (pointer-containing) data. */
  return malloc_kind_tl(lb, NORMAL);
}
#  endif

#  ifdef THREAD_GCJ_FREELISTS
#    include "gc/gc_gcj.h"

GC_API GC_ATTR_MALLOC void *GC_CALL
GC_gcj_malloc(size_t lb, const void *vtable_ptr)
{
  void *result;
  void **tiny_fl;
  size_t lg;

  /*
   * Unlike the other thread-local allocation calls, we assume that the
   * collector has been explicitly initialized.
   */
  GC_ASSERT(GC_gcjobjfreelist != NULL);
#    if defined(USE_PTHREAD_SPECIFIC) || defined(USE_WIN32_SPECIFIC)
  GC_ASSERT(keys_initialized);
#    else
  GC_ASSERT(GC_thread_key != 0);
#    endif

  /*
   * `gcj`-style allocation without locks is extremely tricky.
   * The fundamental issue is that we may end up marking a free list,
   * which has free-list links instead of "vtable" pointers.
   * That is usually OK, since the next object on the free list will be
   * cleared, and will thus be interpreted as containing a zero descriptor.
   * That is fine if the object has not yet been initialized.  But there
   * are interesting potential races.  In the case of incremental
   * collection, this seems hopeless, since the marker may run
   * asynchronously, and may pick up the pointer to the next free-list
   * entry (which it thinks is a "vtable" pointer), get suspended for
   * a while, and then see an allocated object instead of the "vtable".
   * This may be avoidable with either a handshake with the collector or,
   * probably more easily, by moving the free list links to the second
   * "pointer-sized" word of each object.  The latter is not a universal
   * win, since on architecture like Itanium, nonzero offsets are not
   * necessarily free.  And there may be cache fill order issues.
   * For now, we punt with the incremental collection.  This probably means
   * that the incremental collection should be enabled before we create
   * a second thread.
   */
  if (UNLIKELY(GC_incremental))
    return GC_core_gcj_malloc(lb, vtable_ptr, 0 /* `flags` */);

  tiny_fl = ((GC_tlfs)GC_getspecific(GC_thread_key))->gcj_freelists;
  lg = ALLOC_REQUEST_GRANS(lb);

  /*
   * The provided `default_expr` below forces the initialization of the
   * "vtable" pointer.  This is necessary to ensure some very subtle
   * properties required if a garbage collection is run in the middle of
   * such an allocation.  Here we implicitly also assume atomicity for the
   * free list and method pointer assignments.  We must update the free list
   * before we store the pointer.  Otherwise a collection at this point
   * would see a corrupted free list.  A real memory barrier is not needed,
   * since the action of stopping this thread will cause prior writes
   * to complete.  We assert that any concurrent marker will stop us.
   * Thus it is impossible for a mark procedure to see the allocation of the
   * next object, but to see this object still containing a free-list pointer.
   * Otherwise the marker, by misinterpreting the free-list link as a "vtable"
   * pointer, might find a random "mark descriptor" in the next object.
   */
  GC_FAST_MALLOC_GRANS(
      result, lg, tiny_fl, DIRECT_GRANULES, GC_gcj_kind,
      GC_core_gcj_malloc(lb, vtable_ptr, 0 /* `flags` */), do {
        AO_compiler_barrier();
        *(const void **)result = vtable_ptr;
      } while (0));
  return result;
}
#  endif

GC_INNER void
GC_mark_thread_local_fls_for(GC_tlfs p)
{
  int j;

  for (j = 0; j < GC_TINY_FREELISTS; ++j) {
    int kind;

    for (kind = 0; kind < THREAD_FREELISTS_KINDS; ++kind) {
      /*
       * Load the pointer atomically as it might be updated concurrently
       * by `GC_FAST_MALLOC_GRANS()`.
       */
      ptr_t q = GC_cptr_load((volatile ptr_t *)&p->_freelists[kind][j]);

      if (ADDR(q) > HBLKSIZE)
        GC_set_fl_marks(q);
    }
#  ifdef THREAD_GCJ_FREELISTS
    if (LIKELY(j > 0)) {
      ptr_t q = GC_cptr_load((volatile ptr_t *)&p->gcj_freelists[j]);

      if (ADDR(q) > HBLKSIZE)
        GC_set_fl_marks(q);
    }
#  endif
  }
}

#  if defined(GC_ASSERTIONS)
/* Check that all thread-local free-lists in `p` are completely marked. */
void
GC_check_tls_for(GC_tlfs p)
{
  int kind, j;

  for (j = 1; j < GC_TINY_FREELISTS; ++j) {
    for (kind = 0; kind < THREAD_FREELISTS_KINDS; ++kind) {
      GC_check_fl_marks(&p->_freelists[kind][j]);
    }
#    ifdef THREAD_GCJ_FREELISTS
    GC_check_fl_marks(&p->gcj_freelists[j]);
#    endif
  }
}
#  endif

#endif /* THREAD_LOCAL_ALLOC */
