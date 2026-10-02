/*
 * Test that the objects are padded as the mode in effect after the
 * collector initialization requires, including the object of the first
 * allocation, which initializes the collector.  In the
 * all-interior-pointers mode, each collectible object is enlarged by at
 * least a byte (unless the padding is turned off), so that a pointer just
 * past its end is recognized, and the descriptor of `NORMAL` objects (set
 * up by `GC_init`) excludes the last pointer-sized word of an object from
 * the scanning.  At the initialization, `GC_ALL_INTERIOR_POINTERS`
 * environment variable turns the mode on, and `GC_DONT_ADD_BYTE_AT_END`
 * one turns the padding off (or on, if the value is "0").  Each case (a
 * way to set the mode, and the allocation function called first) runs in
 * a child process of its own, as the collector is initialized once per
 * process, except for the case of the unchanged mode, which runs in this
 * process (the only case if `fork()` is unavailable).  Also check that
 * the padding setting cannot be changed after the initialization.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* For `MAX_EXTRA_BYTES` and the availability of `fork()`. */
#define NOT_GCBUILD
#include "private/gc_priv.h"

#if (defined(GC_NO_FINALIZATION) || defined(DBG_HDRS_ALL)) \
    && !defined(NO_TYPED_TEST)
#  define NO_TYPED_TEST
#endif

#ifndef NO_TYPED_TEST
#  include "gc/gc_typed.h"
#endif

/*
 * If `malloc()` is redirected, then the C library (e.g. `setenv()`) might
 * initialize the collector before the first allocation of a case.
 */
#if !defined(HAVE_NO_FORK) && !defined(USE_WINALLOC)   \
    && (!defined(THREADS) || defined(CAN_HANDLE_FORK)) \
    && !defined(REDIRECT_MALLOC)
#  define RUN_CHILD_CASES
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e)                                                            \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/* The ways to set the mode before the collector initialization. */
enum {
  /* Leave the mode as is (as built, or as set by the environment). */
  MODE_AS_IS,
  /*
   * Turn the mode off by `GC_set_all_interior_pointers()` while
   * `GC_ALL_INTERIOR_POINTERS` environment variable turns it on.
   */
  MODE_ALL_INTERIOR_BY_ENV,
  /* Turn the padding off by `GC_set_dont_add_byte_at_end()`. */
  MODE_NO_PADDING,
  /* `GC_DONT_ADD_BYTE_AT_END` environment variable turns the padding off. */
  MODE_NO_PADDING_BY_ENV,
  /*
   * Turn the padding off by `GC_set_dont_add_byte_at_end()` while
   * `GC_DONT_ADD_BYTE_AT_END` environment variable set to "0" turns it on.
   */
  MODE_PADDING_BY_ENV,
  N_MODES
};

/* The allocation functions. */
enum {
  ALLOC_SMALL,
  ALLOC_LARGE,
  ALLOC_ALIGNED,
  ALLOC_MANY,
  ALLOC_UNCOLLECTABLE,
  ALLOC_DEBUG,
  ALLOC_DEBUG2,
  ALLOC_TYPED,
  N_ALLOCS
};

/* A small object size, a multiple of the granule size. */
#define SMALL_LB (2 * GC_GRANULE_BYTES)

/*
 * A size bigger than a half of the biggest supported heap block (64 KB),
 * a multiple of the granule size.
 */
#define LARGE_LB (40 * 1024)

/* A size which is one more than a multiple of the granule size. */
#define ODD_LB (2 * GC_GRANULE_BYTES + 1)

struct obj_s {
  char *p;
  size_t lb;

  /*
   * The offset of the pointer to the target object within the object
   * (or `lb` if none).  The target is collectible and is referenced from
   * this object only.
   */
  size_t ptr_ofs;

  /* The hidden pointer to the target. */
  GC_hidden_pointer target;
};

static void *
checked(void *p)
{
  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}

static unsigned char
fill_byte(size_t i)
{
  return (unsigned char)(i * 7 + 1);
}

/*
 * Fill the client data of `pobj` (except for the pointer to the target)
 * and set the pointer to a new target object.
 */
static void
fill_obj(struct obj_s *pobj, size_t ptr_ofs)
{
  size_t i;

  for (i = 0; i < pobj->lb; i++)
    pobj->p[i] = (char)fill_byte(i);
  pobj->ptr_ofs = ptr_ofs;
  if (ptr_ofs < pobj->lb) {
    /*
     * The target is a large object, thus it is freed by the collection
     * which finds it unreachable.
     */
    const void *target = checked(GC_malloc_atomic(LARGE_LB));

    GC_PTR_STORE_AND_DIRTY(pobj->p + ptr_ofs, target);
    pobj->target = GC_HIDE_POINTER(target);
  }
}

#ifndef NO_TYPED_TEST
/* Allocate a typed object of `lb` bytes with a pointer at its start. */
static void *
typed_malloc(size_t lb)
{
  GC_word bm = 1;

  return GC_malloc_explicitly_typed(lb, GC_make_descriptor(&bm, 1));
}
#endif

/* Allocate an object by the given function. */
static void
alloc_obj(struct obj_s *pobj, int alloc)
{
  char *p = NULL;
  size_t lb = SMALL_LB;
  size_t ptr_ofs = SMALL_LB - sizeof(void *);

  switch (alloc) {
  case ALLOC_SMALL:
    p = (char *)GC_malloc(lb);
    break;
  case ALLOC_LARGE:
    lb = LARGE_LB;
    ptr_ofs = LARGE_LB - sizeof(void *);
    p = (char *)GC_malloc(lb);
    break;
  case ALLOC_ALIGNED:
    p = (char *)GC_memalign(4 * GC_GRANULE_BYTES, lb);
    CHECK(((GC_uintptr_t)p & (4 * GC_GRANULE_BYTES - 1)) == 0);
    break;
  case ALLOC_MANY:
    p = (char *)GC_malloc_many(lb);
    break;
  case ALLOC_UNCOLLECTABLE:
    lb = ODD_LB;
    ptr_ofs = lb;
    p = (char *)GC_malloc_uncollectable(lb);
    break;
  case ALLOC_DEBUG:
  case ALLOC_DEBUG2:
    /*
     * Two sizes, one pointer apart, as the size of the debug header is
     * unknown here.
     */
    lb = ALLOC_DEBUG == alloc ? ODD_LB : ODD_LB + sizeof(void *);
    ptr_ofs = lb;
    p = (char *)GC_debug_malloc(lb, GC_EXTRAS);
    break;
  case ALLOC_TYPED:
#ifdef NO_TYPED_TEST
    pobj->p = NULL;
    return;
#else
    lb = 2 * sizeof(void *) + 1;
    ptr_ofs = 0;
    p = (char *)typed_malloc(lb);
    break;
#endif
  }
  pobj->p = (char *)checked(p);
  pobj->lb = lb;
  fill_obj(pobj, ptr_ofs);
}

/*
 * Check that the object is big enough for the requested size, the
 * padding (`extra_bytes`) and anything the collector stores in it.
 */
static void
check_obj_size(const struct obj_s *pobj, int alloc, size_t extra_bytes)
{
  const char *base;
  size_t need;

  if (NULL == pobj->p)
    return;
  switch (alloc) {
  case ALLOC_SMALL:
  case ALLOC_LARGE:
  case ALLOC_ALIGNED:
  case ALLOC_MANY:
    CHECK(GC_size(pobj->p) >= pobj->lb + extra_bytes);
    if (ALLOC_SMALL == alloc) {
      /* The size is rounded up to a granule only. */
      CHECK(GC_size(pobj->p)
            == ((pobj->lb + extra_bytes + GC_GRANULE_BYTES - 1)
                & ~(size_t)(GC_GRANULE_BYTES - 1)));
    }
    break;
  case ALLOC_UNCOLLECTABLE:
    /* Such objects are not padded. */
    CHECK(GC_size(pobj->p) >= pobj->lb);
    break;
  case ALLOC_DEBUG:
  case ALLOC_DEBUG2:
    base = (const char *)GC_base(pobj->p);
    CHECK(base != NULL);
    need = (size_t)(pobj->p - base) + pobj->lb;
#ifndef SHORT_DBG_HDRS
    /* The end flag (a word) is stored after the client data (aligned). */
    need = ((need + sizeof(GC_uintptr_t) - 1) & ~(sizeof(GC_uintptr_t) - 1))
           + sizeof(GC_uintptr_t);
#endif
    CHECK(GC_size(base) >= need);
    break;
#ifndef NO_TYPED_TEST
  case ALLOC_TYPED:
    /* The descriptor is stored at the end of the object. */
    CHECK(GC_size(pobj->p) >= pobj->lb + sizeof(GC_descr));
    break;
#endif
  }
}

/* Check that the client data and the target object are intact. */
static void
check_obj_contents(const struct obj_s *pobj)
{
  size_t i;

  if (NULL == pobj->p)
    return;
  for (i = 0; i < pobj->lb; i++) {
    if (i >= pobj->ptr_ofs && i < pobj->ptr_ofs + sizeof(void *))
      continue;
    CHECK((unsigned char)pobj->p[i] == fill_byte(i));
  }
  if (pobj->ptr_ofs < pobj->lb) {
    void *target = GC_REVEAL_POINTER(pobj->target);

    CHECK(*(void **)(pobj->p + pobj->ptr_ofs) == target);
    /* The target is not freed. */
    CHECK(GC_base(target) != NULL);
  }
}

#ifdef RUN_CHILD_CASES
/* The all-interior-pointers mode before any change. */
static int initial_all_interior_pointers;
#endif

static void
set_mode(int mode)
{
  if (MODE_AS_IS == mode)
    return;
#ifdef RUN_CHILD_CASES
  (void)unsetenv("GC_ALL_INTERIOR_POINTERS");
  (void)unsetenv("GC_DONT_ADD_BYTE_AT_END");
  switch (mode) {
  case MODE_ALL_INTERIOR_BY_ENV:
    GC_set_all_interior_pointers(0);
    CHECK(setenv("GC_ALL_INTERIOR_POINTERS", "1", 1) == 0);
    break;
  case MODE_NO_PADDING:
    GC_set_dont_add_byte_at_end(1);
    break;
  case MODE_NO_PADDING_BY_ENV:
    CHECK(setenv("GC_DONT_ADD_BYTE_AT_END", "1", 1) == 0);
    break;
  case MODE_PADDING_BY_ENV:
    GC_set_dont_add_byte_at_end(1);
    CHECK(setenv("GC_DONT_ADD_BYTE_AT_END", "0", 1) == 0);
    break;
  }
#endif
}

#ifdef RUN_CHILD_CASES
static void
check_mode(int mode)
{
  int all_interior_pointers = initial_all_interior_pointers;
  int no_padding = MODE_NO_PADDING == mode || MODE_NO_PADDING_BY_ENV == mode;

#  if defined(NO_GETENV) && !defined(CPPCHECK)
  /* The environment variables are ignored.  Only the setters take effect. */
  if (MODE_ALL_INTERIOR_BY_ENV == mode)
    all_interior_pointers = 0;
  no_padding = MODE_NO_PADDING == mode || MODE_PADDING_BY_ENV == mode;
#  else
  if (MODE_ALL_INTERIOR_BY_ENV == mode)
    all_interior_pointers = 1;
#  endif
#  ifdef NO_ALL_INTERIOR_POINTERS
  all_interior_pointers = 0;
#  endif
#  if MAX_EXTRA_BYTES == 0
  /* The padding is turned off at build time. */
  no_padding = 1;
#  endif
  CHECK(GC_get_all_interior_pointers() == all_interior_pointers);
  CHECK(GC_get_dont_add_byte_at_end() == no_padding);
}
#endif

/*
 * Set the mode, allocate an object by each function (by `first` one at
 * first, before `GC_INIT()`), and check the objects across collections.
 */
static void
run_case(int mode, int first)
{
  struct obj_s objs[N_ALLOCS];
  size_t extra_bytes;
  int i;

  set_mode(mode);
  alloc_obj(&objs[first], first);
  GC_INIT();
  for (i = 0; i < N_ALLOCS; i++) {
    if (i != first)
      alloc_obj(&objs[i], i);
  }
#ifdef RUN_CHILD_CASES
  if (mode != MODE_AS_IS)
    check_mode(mode);
#endif
  extra_bytes
      = GC_get_all_interior_pointers() && !GC_get_dont_add_byte_at_end() ? 1
                                                                         : 0;
  for (i = 0; i < N_ALLOCS; i++)
    check_obj_size(&objs[i], i, extra_bytes);
  for (i = 0; i < 3; i++) {
    int j;

    GC_gcollect();
    for (j = 0; j < N_ALLOCS; j++)
      check_obj_contents(&objs[j]);
  }
  GC_reachable_here(objs);
}

static int late_setter_warnings;

static void GC_CALLBACK
count_late_setter_warning(const char *msg, GC_uintptr_t arg)
{
  (void)arg;
  if (strstr(msg, "GC_set_dont_add_byte_at_end") != NULL)
    late_setter_warnings++;
}

/*
 * Check that the padding setting is not changed after the initialization
 * (with a warning).
 */
static void
check_late_setter(void)
{
  GC_warn_proc old_proc = GC_get_warn_proc();
  int no_padding = GC_get_dont_add_byte_at_end();

  GC_set_warn_proc(count_late_setter_warning);
  GC_set_dont_add_byte_at_end(!no_padding);
  GC_set_warn_proc(old_proc);
  CHECK(GC_get_dont_add_byte_at_end() == no_padding);
#if MAX_EXTRA_BYTES > 0
  CHECK(1 == late_setter_warnings);
#endif
}

#ifdef RUN_CHILD_CASES
/* Run a case in a child process.  Return 0 if `fork()` failed. */
static int
run_child_case(int mode, int first)
{
  pid_t pid;
  int status;

  fflush(stdout);
  pid = fork();
  if (-1 == pid) {
    printf("Process fork failed, the remaining child cases are skipped\n");
    return 0;
  }
  if (0 == pid) {
    run_case(mode, first);
    exit(0);
  }
  CHECK(waitpid(pid, &status, 0) == pid);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "Case of mode %d, first allocation %d failed\n", mode,
            first);
    exit(1);
  }
  return 1;
}
#endif

int
main(void)
{
#ifdef RUN_CHILD_CASES
  int mode;
  int first;
  int forked = 1;

  /* The collector is not initialized yet, thus no child inherits it. */
  initial_all_interior_pointers = GC_get_all_interior_pointers();
  for (mode = MODE_AS_IS + 1; mode < N_MODES && forked; mode++) {
    for (first = 0; first < N_ALLOCS && forked; first++)
      forked = run_child_case(mode, first);
  }
#endif
  run_case(MODE_AS_IS, ALLOC_LARGE);
  check_late_setter();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");
  printf("SUCCEEDED\n");
  return 0;
}
