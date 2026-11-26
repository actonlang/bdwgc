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
 * through the address where a walk of the blocks in chunks resumes.  With
 * world-stopped steps and a time limit, the set-up of a full collection
 * clears the mark bits of the blocks in chunks, and the finish of
 * a collection walks the blocks in chunks to start their sweep; between
 * two chunks, the collector keeps the block where the next chunk resumes
 * in a static variable (`clear_next` and `walk_next`), and the collector
 * scans its own static data as a root.
 *
 * The test includes the collector source to run the chunks itself,
 * holding the allocator lock, in the incremental mode with every
 * collection full, and to read the static roots.  It allocates
 * pointer-free objects, each of which takes a block of its own, and keeps
 * only hidden pointers to them, each registered as a disappearing link.
 * At a time limit of 0, every chunk of a walk stops at its first read of
 * the clock, and the test runs the chunks until one stops at the block of
 * an object, but not at the highest one (a collection may leave the
 * address of the highest block in use in a frame).  Then a collection
 * must reclaim that object:
 *   - in the clearing of the mark bits of a set-up, with the objects
 *     unreachable: the collection of the set-up, once the test has
 *     completed the clearing in one chunk without a time limit (thus the
 *     clearing resumed last at that block) and the collection has marked
 *     at once without a time limit;
 *   - in the same clearing: a collection started explicitly instead,
 *     which replaces the set-up;
 *   - in the walk of the blocks of a finish, with the objects reachable
 *     until the walk stops there: the next collection, started explicitly
 *     once the test has dropped the objects and completed the finish
 *     without a time limit (thus the walk resumed last at that block).
 * A case fails if the object is not reclaimed and, at a root scan of that
 * collection, a word of the static roots outside the ranges that the
 * collector excludes from them points into the object.  The procedure set
 * by `GC_set_push_other_roots()` reads these words at each root scan and
 * compares them hidden, so that no copy of the address of the object is
 * left on the stack.  If the object is not reclaimed but no such word
 * points into it, other words keep it alive, such as those of a stack or of
 * the registers, and the case is not checked.  Where `GC_mark_some()` saves
 * the registers of the collecting thread in the static `GC_jmp_buf` before
 * it pushes the roots (with `WRAP_MARK_SOME` and `NO_SEH_AVAILABLE`, as on
 * Windows with a compiler that defines `__GNUC__`), the words of
 * `GC_jmp_buf` are those of the registers, and the test does not count them
 * as words of the static roots.  The test overwrites the stack below its
 * frames after every call of the collector which may leave the address of
 * that block there.  The test is skipped without the parallel marker (only
 * with it the set-up and the finish run in chunks), the incremental mode,
 * the clock or disappearing links, in the leak detection mode, and if no
 * collection would mark in steps.
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

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#if defined(PARALLEL_MARK_DEADLINE) && !defined(GC_NO_FINALIZATION)

#  define CHECK_OUT_OF_MEMORY(p)            \
    do {                                    \
      if (NULL == (p)) {                    \
        fprintf(stderr, "Out of memory\n"); \
        exit(69);                           \
      }                                     \
    } while (0)

/*
 * The number of the objects allocated for each case, enough for several
 * chunks of the walks.
 */
#  define N_OBJS 1024

/*
 * The size of each object, more than half of a block, thus it takes
 * a block of its own and starts where the block starts.
 */
#  define OBJ_BYTES (MAXOBJBYTES + GC_GRANULE_BYTES)

/* The number of the tries of each case. */
#  define MAX_TRIES 4

/* The number of the stack bytes cleared after a call of the collector. */
#  define CLEAR_STACK_BYTES 32768

/* The number of the words of each frame of `clear_stack()`. */
#  define CLEAR_FRAME_WORDS 512

/*
 * The words of the stack between the frame of the caller and that of
 * `make_objects()`, as a frame which does not hold pointers.
 */
#  define PAD_WORDS 64

/*
 * The hidden pointers to the objects, registered as disappearing links,
 * thus each is cleared when its object is found unreachable.
 */
static GC_hidden_pointer links[N_OBJS];

/* The blocks of the objects, as hidden pointers. */
static GC_hidden_pointer blocks[N_OBJS];

/*
 * The objects of the case of the walk of a finish, reachable from here
 * until a chunk of the walk stops at one of them.  The pointers are
 * volatile, so that the stores that drop them are not left out by the
 * compiler.
 */
static void *volatile live[N_OBJS];

/* Whether `make_objects()` keeps the objects in `live`. */
static int keep_objects;

static GC_push_other_roots_proc old_push_other_roots;

/*
 * The object which `push_roots()` checks the static roots for, as an index
 * into `blocks`, or -1 if none.
 */
static int watched = -1;

/*
 * The address of a word of the static roots which pointed into the
 * watched object at a root scan, or `NULL` if no word did.
 */
static void *static_ref;

/*
 * Whether the word at `p` is in the copy of the registers of the collecting
 * thread that `GC_mark_some()` saves in the static `GC_jmp_buf` before it
 * pushes the roots (with `WRAP_MARK_SOME` and `NO_SEH_AVAILABLE`).
 */
#  if defined(WRAP_MARK_SOME) && defined(NO_SEH_AVAILABLE)
#    define IN_REGISTER_COPY(p)      \
      (ADDR_GE(p, (ptr_t)GC_jmp_buf) \
       && ADDR_LT(p, (ptr_t)GC_jmp_buf + sizeof(GC_jmp_buf)))
#  else
#    define IN_REGISTER_COPY(p) FALSE
#  endif

/*
 * Return the address of the first word in `[bottom, top)` that points
 * into the object which starts at the hidden pointer `obj` and has
 * `bytes` bytes, or `NULL` if there is none.  The words are read as
 * `GC_push_all_eager()` reads them, and are compared hidden: a word
 * points into the object if its hidden value is in `(obj - bytes, obj]`.
 * The words of the copy of the registers in `GC_jmp_buf` do not count
 * (see `IN_REGISTER_COPY()`).
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
    if (h <= obj && obj - h < bytes && !IN_REGISTER_COPY(p))
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
 * into the watched object, if any.  `GC_push_roots()` calls this after it
 * has pushed the static roots.
 */
static void GC_CALLBACK
push_roots(void)
{
  if (old_push_other_roots != 0)
    old_push_other_roots();
  if (watched >= 0 && NULL == static_ref)
    static_ref = static_root_pointing_into(blocks[watched], OBJ_BYTES);
}

static void
set_time_limit(unsigned long ms, unsigned long ns)
{
  struct GC_timeval_s tv;

  tv.tv_ms = ms;
  tv.tv_nsec = ns;
  GC_set_time_limit_tv(tv);
}

static void
make_objects(void)
{
  int i;

  /*
   * The objects fit in the heap grown here, thus it does not grow while
   * the collections are disabled.
   */
  if (!GC_expand_hp((size_t)N_OBJS * HBLKSIZE)) {
    fprintf(stderr, "Heap expansion failed\n");
    exit(69);
  }
  /* No collection runs until every object is allocated. */
  GC_disable();
  for (i = 0; i < N_OBJS; i++) {
    void *p = GC_MALLOC_ATOMIC(OBJ_BYTES);

    CHECK_OUT_OF_MEMORY(p);
    /* The link of an object of an earlier case may still be registered. */
    (void)GC_unregister_disappearing_link((void **)&links[i]);
    links[i] = GC_HIDE_POINTER(p);
    blocks[i] = GC_HIDE_POINTER(HBLKPTR(p));
    CHECK(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&links[i], p)
          == GC_SUCCESS);
    live[i] = keep_objects ? p : NULL;
  }
  GC_enable();
}

/* Called through a pointer to prevent inlining. */
static void (*volatile make_objects_p)(void) = make_objects;

/*
 * Call `make_objects()` below a frame of `PAD_WORDS` words, so that the
 * frames of `make_objects()` and of its callees are below the slots of
 * the frames of the other callees of the caller, where `clear_stack()`
 * overwrites them.
 */
static void
make_objects_below(void)
{
  volatile GC_word pad[PAD_WORDS];

  pad[0] = 0;
  make_objects_p();
  pad[PAD_WORDS - 1] = pad[0];
}

static GC_word clear_stack(size_t bytes);

/* These are called through pointers to prevent inlining. */
static void (*volatile make_objects_below_p)(void) = make_objects_below;
static GC_word (*volatile clear_stack_p)(size_t) = clear_stack;

/*
 * Overwrite `bytes` bytes of the stack below the frame of the caller, so
 * that the collector does not find stale pointers to the objects there.
 * This calls itself, each frame overwriting `CLEAR_FRAME_WORDS` words, to
 * keep the frames small.  The returned value is not used.
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

/*
 * Return the index of the object that starts at block `*pnext`, or -1 if
 * there is none or it is the highest one (a collection may leave the
 * address of the highest block in use in a frame of the collector).  The
 * addresses are compared hidden: a lower hidden pointer is a higher
 * address.
 */
static int
object_at(struct hblk *const *pnext)
{
  GC_hidden_pointer h = GC_HIDE_POINTER(*pnext);
  int found = -1;
  int above = 0;
  int i;

  for (i = 0; i < N_OBJS; i++) {
    if (blocks[i] == h) {
      found = i;
    } else if (blocks[i] < h) {
      above = 1;
    }
  }
  return above ? found : -1;
}

/*
 * Start a full collection, as `GC_maybe_gc()` starts it, at a time limit
 * of 0, and run the chunks of its set-up until the clearing of the mark
 * bits stops at the block of an object (see `object_at()`).  Return the
 * index of the object, or -1 if the set-up completes before.
 */
static int
clear_to_object(void)
{
  int i = -1;
  IF_CANCEL(int cancel_state;)

  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(0, 0);
  GC_maybe_gc();
  while (SETUP_SWEEP == setup_phase || SETUP_CLEAR == setup_phase) {
    if (SETUP_CLEAR == setup_phase && (i = object_at(&clear_next)) >= 0)
      break;
    continue_full_setup();
  }
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  return i;
}

/* Complete the clearing in one chunk without a time limit. */
static void
complete_clearing(void)
{
  IF_CANCEL(int cancel_state;)

  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(GC_TIME_UNLIMITED, 0);
  GET_TIME(GC_start_time);
  CHECK(run_setup_chunk());
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/*
 * Run the collection whose set-up is complete, from its first
 * world-stopped mark, which marks at once as there is no time limit, to
 * its finish.
 */
static void
mark_set_up(void)
{
  IF_CANCEL(int cancel_state;)

  LOCK();
  DISABLE_CANCEL(cancel_state);
  CHECK(SETUP_MARK == setup_phase);
  start_full_mark();
  CHECK(!GC_collection_in_progress() && !FINISH_PENDING());
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/*
 * Start a full collection at a time limit of 0, with the objects
 * reachable, and run it until its finish is pending.  Then run the
 * chunks of the finish until the walk of the blocks stops at the block
 * of an object (see `object_at()`).  Return the index of the object, or
 * -1 if the walk completes before, or does not run in chunks.
 */
static int
walk_to_object(void)
{
  int i = -1;
  IF_CANCEL(int cancel_state;)

  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(0, 0);
  GC_maybe_gc();
  while (SETUP_PENDING())
    continue_full_setup();
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  /* The marking proceeds in world-stopped steps, one per call. */
  while (GC_collect_a_little() && !FINISH_PENDING()) {
    /* Empty. */
  }
  LOCK();
  DISABLE_CANCEL(cancel_state);
  /*
   * Empty the detached free lists.  At a time limit of 0, the walk of the
   * blocks starts in the chunk after the one that empties the last list
   * (see `phase_may_start()`).
   */
  while (FINISH_CLEAR == finish_phase || FINISH_WALK_START == finish_phase) {
    GET_TIME(GC_start_time);
    run_finish_chunk(FALSE, TRUE);
  }
  while (FINISH_WALK == finish_phase) {
    if ((i = object_at(&walk_next)) >= 0)
      break;
    GET_TIME(GC_start_time);
    run_finish_chunk(FALSE, TRUE);
  }
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
  return i;
}

/* Complete the pending finish without a time limit. */
static void
complete_finish(void)
{
  IF_CANCEL(int cancel_state;)

  LOCK();
  DISABLE_CANCEL(cancel_state);
  set_time_limit(GC_TIME_UNLIMITED, 0);
  GC_complete_finish();
  RESTORE_CANCEL(cancel_state);
  UNLOCK();
}

/* These are called through pointers to prevent inlining. */
static int (*volatile clear_to_object_p)(void) = clear_to_object;
static void (*volatile complete_clearing_p)(void) = complete_clearing;
static void (*volatile mark_set_up_p)(void) = mark_set_up;
static int (*volatile walk_to_object_p)(void) = walk_to_object;
static void (*volatile complete_finish_p)(void) = complete_finish;

/* Drop the objects, and the copies of their addresses below this frame. */
static void
drop_objects(void)
{
  int i;

  for (i = 0; i < N_OBJS; i++)
    live[i] = NULL;
  (void)clear_stack_p(CLEAR_STACK_BYTES);
}

/* Return the number of the objects not reclaimed. */
static int
n_alive(void)
{
  int n = 0;
  int i;

  for (i = 0; i < N_OBJS; i++) {
    if (links[i] != 0)
      n++;
  }
  return n;
}

/* The cases, in the order of the parts of the description above. */
#  define CASE_CLEARED 0
#  define CASE_REPLACED 1
#  define CASE_WALKED 2

static const char *const case_names[]
    = { "the clearing of the mark bits completed",
        "the set-up replaced by an explicit collection",
        "the walk of the finish completed" };

/*
 * Run case `c` until a walk stops at the block of an object, at most
 * `MAX_TRIES` times.  Return 1 if the object there was reclaimed, 0 if
 * a word of the static roots kept it alive, or -1 if the case is not
 * checked.
 */
static int
run_case(int c)
{
  int tries;

  for (tries = 1; tries <= MAX_TRIES; tries++) {
    int i;
    int n_others;

    /* Complete the collection work left, if any. */
    set_time_limit(GC_TIME_UNLIMITED, 0);
    GC_gcollect();
    keep_objects = CASE_WALKED == c;
    make_objects_below_p();
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    i = CASE_WALKED == c ? walk_to_object_p() : clear_to_object_p();
    (void)clear_stack_p(CLEAR_STACK_BYTES);
    if (i < 0) {
      /* Complete the collection, which reclaims the objects. */
      drop_objects();
      set_time_limit(GC_TIME_UNLIMITED, 0);
      GC_gcollect();
      continue;
    }
    static_ref = NULL;
    if (CASE_CLEARED == c) {
      complete_clearing_p();
      (void)clear_stack_p(CLEAR_STACK_BYTES);
      watched = i;
      mark_set_up_p();
    } else if (CASE_REPLACED == c) {
      watched = i;
      GC_gcollect();
    } else {
      drop_objects();
      complete_finish_p();
      (void)clear_stack_p(CLEAR_STACK_BYTES);
      watched = i;
      GC_gcollect();
    }
    watched = -1;
    n_others = n_alive() - (0 == links[i] ? 0 : 1);
    if (0 == links[i]) {
      printf("%s at the block of object %d (try %d): object reclaimed, %d"
             " of the others not reclaimed\n",
             case_names[c], i, tries, n_others);
      return 1;
    }
    if (static_ref != NULL) {
      printf("%s at the block of object %d (try %d): object not reclaimed,"
             " a word of the static roots at %p points into it, %d of the"
             " others not reclaimed\n",
             case_names[c], i, tries, static_ref, n_others);
      return 0;
    }
    printf("%s at the block of object %d (try %d): not checked (object not"
           " reclaimed, but no word of the static roots points into it), %d"
           " of the others not reclaimed\n",
           case_names[c], i, tries, n_others);
    return -1;
  }
  printf("%s at the block of an object: not checked (no walk stopped there"
         " in %d tries)\n",
         case_names[c], MAX_TRIES);
  return -1;
}

int
main(void)
{
  int c;
  int n_failed = 0;
  int n_checked = 0;
  int n_not_checked = 0;
  GC_bool steps;

  GC_set_manual_vdb_allowed(1);
  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    printf("test skipped\n");
    return 0;
  }
  /* Mark in parallel, if available, although no thread is created. */
  GC_start_mark_threads();
  GC_enable_incremental();
  if (!GC_is_incremental_mode()) {
    printf("test skipped (the incremental mode is off)\n");
    return 0;
  }
  GC_set_full_freq(0);
  /*
   * Without an allocation budget, the heap cap is far above the heap of
   * this test, thus a marking does not complete with its finish in one
   * stop, which it does near the heap cap.
   */
  GC_set_alloc_budget_percent(0);
  LOCK();
  set_time_limit(0, 0);
  steps = steps_on();
  UNLOCK();
  if (!steps) {
    printf("test skipped (no collection marks in steps)\n");
    return 0;
  }
  old_push_other_roots = GC_get_push_other_roots();
  GC_set_push_other_roots(push_roots);
  for (c = CASE_CLEARED; c <= CASE_WALKED; c++) {
    int res = run_case(c);

    if (res < 0) {
      n_not_checked++;
    } else {
      n_checked++;
      if (0 == res)
        n_failed++;
    }
  }
  if (n_failed > 0) {
    fprintf(stderr,
            "%d of %d objects at the block where a walk of the blocks in"
            " chunks resumed last kept alive by a word of the static roots\n",
            n_failed, n_checked);
    exit(1);
  }
  printf("SUCCEEDED (%d cases checked, %d not checked)\n", n_checked,
         n_not_checked);
  return 0;
}

#else

int
main(void)
{
#  ifdef GC_NO_FINALIZATION
  printf("test skipped (no disappearing links)\n");
#  else
  printf("test skipped (the set-up and the finish do not run in chunks)\n");
#  endif
  return 0;
}

#endif
