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
 * through the address where it asks `mmap()` to map its next memory,
 * i.e. the end of its latest mapping.  The test adds heap sections one at
 * a time with `GC_expand_hp()`, and right after each addition allocates
 * a pointer-free object that takes the whole new section, thus starts
 * where the section starts.  Where `mmap()` places each new mapping just
 * below the one before, as Linux does by default, the end of the latest
 * section is the start of the section added before it.  The test keeps
 * only hidden pointers to the objects, each registered as a disappearing
 * link, and checks that the object at the start of the second-latest
 * section is reclaimed within a few collections.  The object at the start
 * of the latest section is not checked, as some builds keep it alive for
 * other reasons.
 * The test runs only on Linux where the collector gets its memory with
 * `mmap()` (not with `sbrk()`) and supports disappearing links.  It is
 * skipped where the latest two sections do not adjoin, in a build without
 * optimization, and where `INCLUDE_LINUX_THREAD_DESCR` is defined (with
 * threads and `malloc()` redirected).  Without optimization, the frames of
 * the collector can hold the address of a heap block, which is that of its
 * first object, when it scans the stack.  With `INCLUDE_LINUX_THREAD_DESCR`,
 * the collector adds the mapping that holds the thread-local storage of
 * the main thread as a root, and that mapping can include the memory where
 * the collector records the start of each heap section.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(LINUX) && defined(USE_MMAP) && defined(__OPTIMIZE__) \
    && !defined(GC_NO_FINALIZATION) && !defined(INCLUDE_LINUX_THREAD_DESCR)

#  define TEST_ASSERT(e)                                                    \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }

#  define CHECK_OUT_OF_MEMORY(p)            \
    do {                                    \
      if (NULL == (p)) {                    \
        fprintf(stderr, "Out of memory\n"); \
        exit(69);                           \
      }                                     \
    } while (0)

/* The number of the heap sections added. */
#  define N_SECTIONS 8

/*
 * The size of each added heap section, a multiple of the heap block size
 * and of the page size.
 */
#  define SECTION_BYTES ((size_t)1 << 20)

/*
 * The size of each object.  The heap blocks of the object take the whole
 * section, as the object is less than a block smaller than the section,
 * also with the extra bytes and the debug header of the collector.
 */
#  define OBJ_BYTES (SECTION_BYTES - 1024)

/*
 * The number of the collections within which the checked objects should
 * be reclaimed (more than one in case a stale copy of a pointer to one of
 * them is found by the collector).
 */
#  define MAX_COLLECTIONS 10

/* The number of the stack bytes cleared after the objects are made. */
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
static GC_hidden_pointer links[N_SECTIONS];

static void
make_objects(void)
{
  int i;

  /* No collection runs until every object is allocated. */
  GC_disable();
  for (i = 0; i < N_SECTIONS; i++) {
    const void *p;

    if (!GC_expand_hp(SECTION_BYTES)) {
      fprintf(stderr, "Heap expansion %d failed\n", i);
      exit(1);
    }
    p = GC_MALLOC_ATOMIC(OBJ_BYTES);
    CHECK_OUT_OF_MEMORY(p);
    links[i] = GC_HIDE_POINTER(p);
    /*
     * The macro registers the link for the base address of the object,
     * which is that of the debug header of the object if `GC_DEBUG` is
     * defined.
     */
    TEST_ASSERT(GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&links[i], p)
                == GC_SUCCESS);
  }
  GC_enable();
}

/* Called through a pointer to prevent inlining. */
static void (*volatile make_objects_p)(void) = make_objects;

/*
 * Call `make_objects()` below a frame of `PAD_WORDS` words.  A frame of
 * another callee of the caller, e.g. of `GC_gcollect()`, may have a slot
 * which it does not write, which keeps the value an earlier frame at its
 * place left there.  The frames of `make_objects()` and of its callees are
 * below such slots, where `clear_stack()` overwrites them.
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
 * Overwrite `bytes` bytes of the stack below the frame of the caller,
 * where `make_objects()` and the allocation calls ran, so that the
 * collector does not find stale pointers to the objects there.  This
 * calls itself, each frame overwriting `CLEAR_FRAME_WORDS` words, to keep
 * the frames small.  The returned value is not used.
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
  int n_collections;

  GC_INIT();
  if (GC_get_find_leak()) {
    printf("This test program is not designed for leak detection mode\n");
    printf("test skipped\n");
    return 0;
  }
  make_objects_below_p();
  (void)clear_stack_p(CLEAR_STACK_BYTES);
  /*
   * Skip unless the latest section is just below the one added before
   * it.  The pointers are compared hidden, so that no copy of them is
   * left on the stack: a pointer one section higher is a hidden pointer
   * one section lower.
   */
  if (links[N_SECTIONS - 1] - SECTION_BYTES != links[N_SECTIONS - 2]) {
    printf("test skipped (the latest two heap sections do not adjoin)\n");
    return 0;
  }
  for (n_collections = 0;
       n_collections < MAX_COLLECTIONS && links[N_SECTIONS - 2] != 0;
       n_collections++) {
    GC_gcollect();
  }
  if (links[N_SECTIONS - 2] != 0) {
    fprintf(stderr,
            "Object at the start of the second-latest heap section not"
            " reclaimed in %d collections\n",
            MAX_COLLECTIONS);
    exit(1);
  }
  printf("SUCCEEDED (the object at the start of the second-latest heap"
         " section reclaimed in %d collections, heap size: %lu KiB)\n",
         n_collections, (unsigned long)(GC_get_heap_size() >> 10));
  return 0;
}

#else

int
main(void)
{
#  if defined(GC_NO_FINALIZATION)
  printf("test skipped (no disappearing links)\n");
#  elif defined(LINUX) && defined(USE_MMAP) && !defined(__OPTIMIZE__)
  printf("test skipped (built without optimization)\n");
#  elif defined(INCLUDE_LINUX_THREAD_DESCR)
  printf("test skipped (INCLUDE_LINUX_THREAD_DESCR)\n");
#  else
  printf("test skipped (the heap is not mapped with mmap on Linux)\n");
#  endif
  return 0;
}

#endif
