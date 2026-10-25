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
 * Test that a child process forked by a process with parallel markers
 * does not take the mark counts of the blocks for exact ones.  The
 * markers increment the count of a block without synchronization, thus
 * a count may be lower than the number of the set mark bits, and the
 * collector never decrements a count to zero (which means a block without
 * marked objects) while the counts may be low.  The parent process clears
 * the mark bits of uncollectable objects (whose mark bits no collection
 * clears), except those of the first objects of each block (the kept
 * ones, 1 up to half of the block, varying by block), and sets them
 * again from two threads at once, as parallel markers would, then forks.
 * The child process, which marks without the markers, frees the objects
 * explicitly, except the kept ones, and collects: clearing the mark bits
 * of the freed objects on the free list decrements the counts.  A count
 * decremented to zero would free the block of the kept objects (or,
 * decremented further, wrap around).  This is repeated with other kept
 * objects in three child processes.  The test fails if a kept object is
 * no longer an object of the heap, or if its contents change while the
 * child process allocates objects of its size, or if a child process
 * crashes (an assertion in a build with assertions).  Without the
 * parallel markers or the support of `fork()`, the test is skipped.
 */

#ifdef HAVE_CONFIG_H
/* For `GC_THREADS` and `HANDLE_FORK` macros. */
#  include "config.h"
#endif

#undef GC_NO_THREAD_REDIRECTS
#define NOT_GCBUILD
#include "private/gc_priv.h"

#include "gc/gc_mark.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(GC_PTHREADS) && !defined(GC_WIN32_PTHREADS)   \
    && defined(CAN_HANDLE_FORK) && defined(PARALLEL_MARK) \
    && defined(AO_HAVE_load) && defined(AO_HAVE_store)    \
    && defined(AO_HAVE_fetch_and_add1)

#  include <pthread.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>

#  define CHECK(e)                                                            \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #e); \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

/* The objects, of `OBJ_WORDS` words each. */
#  define N_OBJS 400000
#  define OBJ_WORDS 4

/* The threads which set the mark bits at once. */
#  define N_SETTERS 2

/* The rounds of clearing and setting the mark bits before each fork. */
#  define N_ROUNDS 4

/* The child processes. */
#  define N_FORKS 3

/* The most objects kept per block, plus one. */
#  define KEPT_MAX (HBLKSIZE / (2 * OBJ_WORDS * sizeof(GC_word)))

static GC_word *objs[N_OBJS];

/* The number of the block of the object, and its place in the block. */
static size_t block_no[N_OBJS];
static size_t place[N_OBJS];

/* Whether the object is kept (not freed by the child process). */
static char kept[N_OBJS];

static volatile AO_t n_ready;
static volatile AO_t go;

static void *
set_marks(void *arg)
{
  size_t t = (size_t)(GC_word)arg;
  size_t i;

  (void)AO_fetch_and_add1(&n_ready);
  while (!AO_load(&go)) {
    /* Empty. */
  }
  /* Every setter visits the blocks in the same order. */
  for (i = t; i < N_OBJS; i += N_SETTERS) {
    if (!kept[i])
      GC_set_mark_bit(objs[i]);
  }
  return NULL;
}

static void
fill(GC_word *p, GC_word v)
{
  size_t j;

  for (j = 0; j < OBJ_WORDS; j++)
    p[j] = v;
}

static void
check_kept(const char *when)
{
  size_t i;

  for (i = 0; i < N_OBJS; i++) {
    size_t j;

    if (!kept[i])
      continue;
    if (GC_base(objs[i]) != objs[i]) {
      fprintf(stderr, "Kept object %lu is no longer in the heap %s\n",
              (unsigned long)i, when);
      exit(1);
    }
    for (j = 0; j < OBJ_WORDS; j++) {
      if (objs[i][j] != (GC_word)i) {
        fprintf(stderr, "Kept object %lu overwritten %s\n", (unsigned long)i,
                when);
        exit(1);
      }
    }
  }
}

static void
fork_and_check(void)
{
  int status;
  pid_t pid;
  size_t i;

  GC_atfork_prepare();
  pid = fork();
  if (0 == pid) {
    GC_atfork_child();
    for (i = 0; i < N_OBJS; i++) {
      if (!kept[i]) {
        GC_FREE(objs[i]);
        objs[i] = NULL;
      }
    }
    GC_gcollect();
    check_kept("after the collection");
    /* Objects of the same size reuse the freed memory. */
    for (i = 0; i < 2 * N_OBJS; i++) {
      GC_word *p
          = (GC_word *)GC_MALLOC_UNCOLLECTABLE(OBJ_WORDS * sizeof(GC_word));

      CHECK(p != NULL);
      fill(p, ~(GC_word)0);
    }
    check_kept("after the allocation");
    exit(0);
  }
  GC_atfork_parent();
  CHECK(pid != -1);
  CHECK(waitpid(pid, &status, 0) == pid);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "Child process failed, status 0x%x\n", status);
    exit(1);
  }
}

int
main(void)
{
  size_t i, n_blocks = 0;
  int n;

  GC_INIT();
  GC_start_mark_threads();
  if (0 == GC_get_parallel()) {
    printf("No parallel markers, test skipped\n");
    return 0;
  }
  for (i = 0; i < N_OBJS; i++) {
    objs[i] = (GC_word *)GC_MALLOC_UNCOLLECTABLE(OBJ_WORDS * sizeof(GC_word));
    CHECK(objs[i] != NULL);
    fill(objs[i], (GC_word)i);
    if (0 == i || HBLKPTR(objs[i]) != HBLKPTR(objs[i - 1])) {
      n_blocks++;
      place[i] = 0;
    } else {
      place[i] = place[i - 1] + 1;
    }
    block_no[i] = n_blocks;
  }
  GC_gcollect();

  for (n = 0; n < N_FORKS; n++) {
    int round;

    for (i = 0; i < N_OBJS; i++)
      kept[i] = place[i] <= (block_no[i] + 61 * (size_t)n) % KEPT_MAX;
    /* No collection runs while the mark bits are set by the client. */
    GC_disable();
    for (round = 0; round < N_ROUNDS; round++) {
      pthread_t th[N_SETTERS];
      size_t t;

      for (i = 0; i < N_OBJS; i++) {
        if (!kept[i])
          GC_clear_mark_bit(objs[i]);
      }
      AO_store(&n_ready, 0);
      AO_store(&go, 0);
      for (t = 0; t < N_SETTERS; t++)
        CHECK(0
              == pthread_create(&th[t], NULL, set_marks, (void *)(GC_word)t));
      while (AO_load(&n_ready) < N_SETTERS) {
        /* Empty. */
      }
      AO_store(&go, 1);
      for (t = 0; t < N_SETTERS; t++)
        CHECK(0 == pthread_join(th[t], NULL));
    }
    GC_enable();
    for (i = 0; i < N_OBJS; i++)
      CHECK(GC_is_marked(objs[i]));
    fork_and_check();
  }
  printf("SUCCEEDED (%lu blocks, %d child processes)\n",
         (unsigned long)n_blocks, N_FORKS);
  return 0;
}

#else

int
main(void)
{
  printf("test skipped\n");
  return 0;
}

#endif
