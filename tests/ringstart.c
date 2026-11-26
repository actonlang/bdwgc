/*
 * Copyright (c) 2026 Acton contributors
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program for any purpose,
 * provided the above notices are retained on all copies.  Permission to
 * modify the code and to distribute modified code is granted, provided the
 * above notices are retained, and a notice that the code was modified is
 * included with the above copyright notice.
 */

/*
 * Test that the collector does not keep an unreachable object alive
 * through the address of a ring of a parallel mark phase once the ring is
 * recycled into the heap.  With the parallel marker and
 * `STEAL_MARK_STACK_RANGES`, a parallel mark phase uses the global mark
 * stack as a ring, which the markers find by its size, and the collector
 * scans its own static data as a root.  When the global mark stack is
 * replaced by a larger one, `GC_after_marking()` recycles the old one into
 * the heap.
 *
 * The test includes the collector source to replace the global mark stack,
 * to clear the black lists and to read the static roots.  It lets
 * a collection mark in a parallel mark phase with all the markers, then
 * replaces the global mark stack by one twice as large, as a marking does
 * where the mark stack overflowed, and recycles the old one into the heap,
 * where it becomes a free block, or the end of one if a free block ends
 * where it starts.  The test allocates a pointer-free object of almost the
 * size of that free block, thus the object starts where the free block
 * starts and contains the start of the old mark stack, and keeps only
 * a hidden pointer to it, registered as a disappearing link.  The object
 * must be reclaimed within a few collections.  The test fails if it is not
 * and, at a root scan of the last collection, a word of the static roots
 * outside the ranges that the collector excludes from them points into the
 * object.  The procedure set by `GC_set_push_other_roots()` reads these
 * words at each root scan and compares them hidden, so that no copy of the
 * address of the object is left on the stack.  If the object is not
 * reclaimed but no such word points into it, other words keep it alive,
 * such as those of a stack or of the registers, and the object is not
 * checked.  Where `GC_mark_some()` saves the registers of the collecting
 * thread in the static `GC_jmp_buf` before it pushes the roots (with
 * `WRAP_MARK_SOME` and `NO_SEH_AVAILABLE`, as on Windows with a compiler
 * that defines `__GNUC__`), the words of `GC_jmp_buf` are those of the
 * registers, and the test does not count them as words of the static roots.
 * The black lists are cleared before the allocation: where the old mark
 * stack lies within the bounds of the heap (where memory is mapped
 * top-down, as on Linux), a stale pointer to it black-lists its first page
 * at every collection, and the allocator puts the blocks of an object over
 * a black-listed page only for pointer-free objects of up to two blocks and
 * for uncollectable objects.  The test is skipped without the parallel
 * marker (or `STEAL_MARK_STACK_RANGES`), without disappearing links, in the
 * leak detection mode, where `INCLUDE_LINUX_THREAD_DESCR` is defined (with
 * threads and `malloc()` redirected), where no collection marks in
 * a parallel mark phase, and where the object does not start where the old
 * mark stack starts and the collector does not recognize interior pointers.
 * With `INCLUDE_LINUX_THREAD_DESCR`, the collector adds the mapping that
 * holds the thread-local storage of the main thread as a root, and that
 * mapping can include the memory where the collector keeps the headers of
 * the blocks and records the start of each heap section.  The header of
 * a block holds the address of the block.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define GC_SINGLE_OBJ_BUILD
#if !defined(__cplusplus) && !defined(GC_VAR_ATTR)
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
#if GC_GNUC_PREREQ(4, 6) && defined(MSWIN32) && !defined(NO_DEBUGGING) \
    && !defined(LINT2)
/*
 * In this configuration `ABORT()` returns (after `DebugBreak()`), thus
 * gcc (mingw-w64) may follow a failed assertion of the collector, e.g.
 * that `kind` is less than `MAXOBJKINDS`, and report an index out of the
 * bounds of an array after it.
 */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#  define ARRAY_BOUNDS_IGNORED
#endif
#include "../extra/gc.c"
#ifdef ARRAY_BOUNDS_IGNORED
#  pragma GCC diagnostic pop
#endif
#if defined(CPPCHECK) && GC_GNUC_PREREQ(12, 0)
#  pragma GCC diagnostic pop
#endif

#if defined(PARALLEL_MARK) && defined(STEAL_MARK_STACK_RANGES) \
    && !defined(GC_NO_FINALIZATION) && !defined(INCLUDE_LINUX_THREAD_DESCR)

#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
                #e);                                                          \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define CHECK_OUT_OF_MEMORY(p)            \
    do {                                    \
      if (NULL == (p)) {                    \
        fprintf(stderr, "Out of memory\n"); \
        exit(69);                           \
      }                                     \
    } while (0)

/*
 * The number of the allocations, at most, until the object starts where
 * the free block of the old mark stack starts.
 */
#  define MAX_TRIES 8

/*
 * The number of the collections within which the object should be
 * reclaimed (more than one in case a stale copy of a pointer to it is
 * found by the collector).
 */
#  define MAX_COLLECTIONS 5

/* The number of the stack bytes cleared after the object is made. */
#  define CLEAR_STACK_BYTES 32768

/* The number of the words of each frame of `clear_stack()`. */
#  define CLEAR_FRAME_WORDS 512

/*
 * The hidden pointer to the object, registered as a disappearing link,
 * thus it is cleared when the object is found unreachable.
 */
static GC_hidden_pointer link_obj;

/*
 * The old mark stack, and the free block that it becomes a part of, as
 * hidden pointers.
 */
static GC_hidden_pointer old_stack;
static GC_hidden_pointer free_start;

static GC_push_other_roots_proc old_push_other_roots;

/*
 * The size of the object which `push_roots()` checks the static roots for,
 * or 0 if none.  The object starts at `free_start`.
 */
static size_t watched_bytes;

/*
 * The address of a word of the static roots which pointed into the object
 * at a root scan of the latest collection, or `NULL` if no word did.
 */
static void *static_ref;

/*
 * Whether the word at `p` is in the copy of the registers of the collecting
 * thread that `GC_mark_some()` saves in the static `GC_jmp_buf` before it
 * pushes the roots (with `WRAP_MARK_SOME` and `NO_SEH_AVAILABLE`).
 */
static GC_bool
in_register_copy(ptr_t p)
{
#  if defined(WRAP_MARK_SOME) && defined(NO_SEH_AVAILABLE)
  return ADDR_GE(p, (ptr_t)GC_jmp_buf)
         && ADDR_LT(p, (ptr_t)GC_jmp_buf + sizeof(GC_jmp_buf));
#  else
  UNUSED_ARG(p);
  return FALSE;
#  endif
}

/*
 * Return the address of the first word in `[bottom, top)` that points
 * into the object which starts at the hidden pointer `obj` and has
 * `bytes` bytes, or `NULL` if there is none.  The words are read as
 * `GC_push_all_eager()` reads them, and are compared hidden: a word
 * points into the object if its hidden value is in `(obj - bytes, obj]`.
 * The words of the copy of the registers in `GC_jmp_buf` do not count
 * (see `in_register_copy()`).
 */
GC_ATTR_NO_SANITIZE_ADDR_MEM_THREAD
static void *
range_pointing_into(ptr_t bottom, ptr_t top, GC_hidden_pointer obj,
                    size_t bytes)
{
  ptr_t p = PTR_ALIGN_UP(bottom, ALIGNMENT);
  word lim_addr = ADDR(PTR_ALIGN_DOWN(top, ALIGNMENT)) - sizeof(ptr_t);

  for (; ADDR(p) <= lim_addr; p += ALIGNMENT) {
    ptr_t q;
    GC_hidden_pointer h;

    LOAD_PTR_OR_CONTINUE(q, p);
    h = GC_HIDE_POINTER(q);
    if (h <= obj && obj - h < bytes && !in_register_copy(p))
      return p;
  }
  return NULL;
}

/*
 * Return the address of a word of the registered static roots, outside
 * the ranges that the collector excludes from them, which points into the
 * object that starts at the hidden pointer `obj` and has `bytes` bytes, or
 * `NULL` if there is none.  The ranges are skipped as
 * `GC_push_conditional_with_exclusions()` skips them, and the words are
 * compared by `range_pointing_into()`.
 */
static void *
static_root_pointing_into(GC_hidden_pointer obj, size_t bytes)
{
  size_t i;

  for (i = 0; i < n_root_sets; i++) {
    ptr_t bottom = GC_static_roots[i].r_start;
    ptr_t top = GC_static_roots[i].r_end;

    while (ADDR_LT(bottom, top)) {
      struct exclusion *next = GC_next_exclusion(bottom);
      ptr_t excl_start = top;

      if (next != NULL) {
        if (ADDR_GE(next->e_start, top)) {
          next = NULL;
        } else {
          excl_start = next->e_start;
        }
      }
      if (ADDR_LT(bottom, excl_start)) {
        void *p = range_pointing_into(bottom, excl_start, obj, bytes);

        if (p != NULL)
          return p;
      }
      if (NULL == next)
        break;
      bottom = next->e_end;
    }
  }
  return NULL;
}

/*
 * Push the other roots, then check the static roots for a word that points
 * into the object, if it is watched.  `GC_push_roots()` calls this after
 * it has pushed the static roots.
 */
static void GC_CALLBACK
push_roots(void)
{
  if (old_push_other_roots != 0)
    old_push_other_roots();
  if (watched_bytes > 0 && NULL == static_ref)
    static_ref = static_root_pointing_into(free_start, watched_bytes);
}

/*
 * Return the free block that contains `p`, or `NULL` if there is none.
 * The blocks of a free block after its first one have no header.
 */
static struct hblk *
free_block_containing(ptr_t p)
{
  struct hblk *h = HBLKPTR(p);
  hdr *hhdr;

  while (NULL == (hhdr = HDR(h))) {
    if (ADDR_LT((ptr_t)h, (ptr_t)GC_least_plausible_heap_addr))
      return NULL;
    h--;
  }
  if (IS_FORWARDING_ADDR_OR_NIL(hhdr) || !HBLK_IS_FREE(hhdr)
      || !ADDR_LT(p, (ptr_t)h + hhdr->hb_sz))
    return NULL;
  return h;
}

/*
 * Replace the global mark stack by one twice as large, and recycle the old
 * one into the heap.  Return the size of the free block that the old one
 * becomes a part of, or 0 if it is in none.
 */
static size_t
replace_mark_stack(void)
{
  mse *old;
  struct hblk *h;
  size_t res = 0;

  LOCK();
  old = GC_mark_stack;
  old_stack = GC_HIDE_POINTER(old);
  alloc_mark_stack(2 * GC_mark_stack_size);
  if (GC_mark_stack != old) {
    GC_after_marking();
    /* It is merged with a free block that ends where it starts, if any. */
    h = free_block_containing((ptr_t)old);
    if (h != NULL) {
      free_start = GC_HIDE_POINTER(h);
      res = HDR(h)->hb_sz;
    }
    /*
     * The allocator puts no pointer-free object of more than two blocks
     * over a black-listed page.
     */
    GC_clear_bl(GC_old_stack_bl);
    GC_clear_bl(GC_incomplete_stack_bl);
    if (!GC_all_interior_pointers) {
      GC_clear_bl(GC_old_normal_bl);
      GC_clear_bl(GC_incomplete_normal_bl);
    }
  }
  UNLOCK();
  return res;
}

/*
 * Allocate pointer-free objects of `bytes` bytes until one starts where
 * the free block of the old mark stack starts, `MAX_TRIES` at most, and
 * register a disappearing link for it.  Return whether one starts there.
 */
static int
make_object(size_t bytes)
{
  int found = 0;
  int i;

  /*
   * Room in the heap for the link besides the object, thus the heap does
   * not grow while the collections are disabled.
   */
  if (!GC_expand_hp(16 * (size_t)HBLKSIZE)) {
    fprintf(stderr, "Heap expansion failed\n");
    exit(69);
  }
  /* No collection runs until the object is allocated. */
  GC_disable();
  for (i = 0; i < MAX_TRIES && !found; i++) {
    void *p = GC_MALLOC_ATOMIC(bytes);

    CHECK_OUT_OF_MEMORY(p);
    if (GC_HIDE_POINTER(p) == free_start) {
      link_obj = GC_HIDE_POINTER(p);
      CHECK(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&link_obj, p)
            == GC_SUCCESS);
      found = 1;
    }
  }
  GC_enable();
  return found;
}

static GC_word clear_stack(size_t bytes);

/* These are called through pointers to prevent inlining. */
static size_t (*volatile replace_mark_stack_p)(void) = replace_mark_stack;
static int (*volatile make_object_p)(size_t) = make_object;
static GC_word (*volatile clear_stack_p)(size_t) = clear_stack;

/*
 * Overwrite `bytes` bytes of the stack below the frame of the caller,
 * where `make_object()` and the allocation calls ran, so that the
 * collector does not find stale pointers to the object there.  This calls
 * itself, each frame overwriting `CLEAR_FRAME_WORDS` words, to keep the
 * frames small.  The returned value is not used.
 */
static GC_word
clear_stack(size_t bytes)
{
  volatile GC_word words[CLEAR_FRAME_WORDS];
  size_t i;

  for (i = 0; i < CLEAR_FRAME_WORDS; i++)
    words[i] = 0;
  /* The result is stored, thus this is not a tail call. */
  if (bytes > sizeof(words))
    words[0] = clear_stack_p(bytes - sizeof(words));
  return words[0];
}

int
main(void)
{
  word mark_no;
  size_t block_bytes;
  size_t obj_bytes;
  int n_collections;
  const char *where;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    printf("test skipped\n");
    return 0;
  }
  /* Mark in parallel, if available, although no thread is created. */
  GC_start_mark_threads();
  if (!GC_get_parallel()) {
    printf("test skipped (no parallel marker)\n");
    return 0;
  }
  /* Every mark phase has all the markers. */
  GC_set_bytes_per_marker(0);
  mark_no = GC_mark_no;
  GC_gcollect();
  if (GC_mark_no == mark_no) {
    printf("test skipped (no parallel mark phase)\n");
    return 0;
  }
  block_bytes = replace_mark_stack_p();
  if (0 == block_bytes) {
    printf("test skipped (the old mark stack is in no free block)\n");
    return 0;
  }
  /*
   * The object contains the start of the old mark stack, at its own start
   * unless the old mark stack was merged with a free block below it.
   */
  where = free_start == old_stack ? "at" : "over";
  if (free_start != old_stack && !GC_all_interior_pointers) {
    printf("test skipped (the free block of the old mark stack starts below"
           " it, and interior pointers are not recognized)\n");
    return 0;
  }
  obj_bytes = block_bytes - HBLKSIZE;
  if (!make_object_p(obj_bytes)) {
    printf("test skipped (no object starts where the free block of the old"
           " mark stack starts)\n");
    return 0;
  }
  (void)clear_stack_p(CLEAR_STACK_BYTES);
  old_push_other_roots = GC_get_push_other_roots();
  GC_set_push_other_roots(push_roots);
  watched_bytes = obj_bytes;
  for (n_collections = 0; n_collections < MAX_COLLECTIONS && link_obj != 0;
       n_collections++) {
    static_ref = NULL;
    GC_gcollect();
  }
  watched_bytes = 0;
  if (link_obj != 0) {
    if (static_ref != NULL) {
      fprintf(stderr,
              "Object %s the start of the recycled mark stack not reclaimed"
              " in %d collections: a word of the static roots at %p points"
              " into it\n",
              where, MAX_COLLECTIONS, static_ref);
      exit(1);
    }
    printf("test not checked (the object %s the start of the recycled mark"
           " stack was not reclaimed in %d collections, but no word of the"
           " static roots points into it)\n",
           where, MAX_COLLECTIONS);
    return 0;
  }
  printf("SUCCEEDED (the object %s the start of the recycled mark stack,"
         " in a free block of %lu KiB, reclaimed in %d collections)\n",
         where, (unsigned long)(block_bytes >> 10), n_collections);
  return 0;
}

#else

int
main(void)
{
#  ifdef GC_NO_FINALIZATION
  printf("test skipped (no disappearing links)\n");
#  elif !defined(PARALLEL_MARK) || !defined(STEAL_MARK_STACK_RANGES)
  printf("test skipped (no parallel mark phase uses a ring)\n");
#  else
  printf("test skipped (INCLUDE_LINUX_THREAD_DESCR)\n");
#  endif
  return 0;
}

#endif
