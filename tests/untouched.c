/*
 * Test that the incremental (generational) collection notices a pointer
 * stored to a page of an old object if the page has not been written
 * since the memory was obtained from the OS.  The objects are allocated
 * before and after the incremental mode is turned on; after a collection
 * (which makes them old), the only pointer to a new object is stored to
 * each such page, then a partial collection is done, and the new objects
 * are checked to be alive.  This fails if the virtual dirty bit
 * implementation misses the first write to a page which is not populated
 * yet (e.g. `userfaultfd` write protection does not apply to such pages)
 * and the collector does not populate the pages itself.
 */

#include <stdio.h>
#include <stdlib.h>

#include "gc.h"

#define TEST_ASSERT(e)                                                    \
  if (!(e)) {                                                             \
    fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
            #e);                                                          \
    exit(1);                                                              \
  }

#define CHECK_OUT_OF_MEMORY(p)            \
  do {                                    \
    if (NULL == (p)) {                    \
      fprintf(stderr, "Out of memory\n"); \
      exit(69);                           \
    }                                     \
  } while (0)

/*
 * The distance between the pointers stored to a big object, i.e. the
 * minimum page size.
 */
#define STORE_STEP 4096

/*
 * Large objects spanning many pages (big enough so that a write to
 * a nearby object does not populate all their pages as a part of
 * a transparent huge page).
 */
#define N_BIG 8
#define BIG_BYTES ((size_t)256 * STORE_STEP)

/*
 * Objects bigger than a page (these are small ones if `HBLKSIZE` is
 * bigger than the page size); a pointer is stored to the last word.
 */
#define N_MEDIUM 32
#define MEDIUM_BYTES 6000

#define N_ROUNDS 2

#define N_LINKS (N_BIG * (BIG_BYTES / STORE_STEP - 2) + N_MEDIUM)

#define CHILD_WORDS 4

#define MAGIC ((GC_word)0x5a5a1234)

/* The objects of each round (the arrays are the roots). */
static void **big[N_ROUNDS][N_BIG];
static void **medium[N_ROUNDS][N_MEDIUM];

/* The hidden pointers to the children, cleared if a child is collected. */
static GC_hidden_pointer links[N_ROUNDS][N_LINKS];

/* The location of the pointer to each child. */
static void **slots[N_ROUNDS][N_LINKS];

static void
alloc_objects(int round)
{
  int i;

  /*
   * The heap has been expanded, thus these are likely allocated from
   * the memory not touched since it was obtained from the OS (and not
   * cleared by the collector if it is known to be zero-filled).
   */
  for (i = 0; i < N_BIG; i++) {
    big[round][i] = (void **)GC_MALLOC(BIG_BYTES);
    CHECK_OUT_OF_MEMORY(big[round][i]);
  }
  for (i = 0; i < N_MEDIUM; i++) {
    medium[round][i] = (void **)GC_MALLOC(MEDIUM_BYTES);
    CHECK_OUT_OF_MEMORY(medium[round][i]);
  }
}

static void
store_child(int round, int n, void **slot)
{
  GC_word *q = (GC_word *)GC_MALLOC(CHILD_WORDS * sizeof(GC_word));

  CHECK_OUT_OF_MEMORY(q);
  q[0] = MAGIC ^ (GC_word)n;
  q[CHILD_WORDS - 1] = MAGIC + (GC_word)round;
  links[round][n] = GC_HIDE_POINTER(q);
#ifndef GC_NO_FINALIZATION
  /*
   * The disappearing links are not supported in the find-leak mode (the
   * objects are not reclaimed in that mode anyway).
   */
  if (!GC_get_find_leak()) {
    TEST_ASSERT(
        GC_GENERAL_REGISTER_DISAPPEARING_LINK((void **)&links[round][n], q)
        == GC_SUCCESS);
  }
#endif
  slots[round][n] = slot;
  /*
   * This is the only reference to the child.  The store is reported to
   * the collector in case of the manual VDB mode (in the other modes,
   * this is just a store).
   */
  GC_ptr_store_and_dirty(slot, q);
}

static void
store_children(int round)
{
  int i, n = 0;

  for (i = 0; i < N_BIG; i++) {
    size_t ofs;

    /*
     * Skip the first and the last page of the object (these might be
     * touched by assertion checks of the collector).
     */
    for (ofs = STORE_STEP; ofs < BIG_BYTES - STORE_STEP; ofs += STORE_STEP)
      store_child(round, n++, &big[round][i][ofs / sizeof(void *)]);
  }
  for (i = 0; i < N_MEDIUM; i++) {
    store_child(round, n++,
                &medium[round][i][MEDIUM_BYTES / sizeof(void *) - 1]);
  }
  TEST_ASSERT((int)N_LINKS == n);
}

static void
collect_partially(void)
{
  if (GC_is_incremental_mode()) {
    GC_start_incremental_collection();
    while (GC_collect_a_little()) {
      /* Empty. */
    }
  } else {
    GC_gcollect();
  }
}

static void
check_children(int round)
{
  int n, n_lost = 0;

  for (n = 0; n < (int)N_LINKS; n++) {
    const GC_word *q;

    if (0 == links[round][n]) {
      n_lost++;
      continue;
    }
    q = (GC_word *)GC_REVEAL_POINTER(links[round][n]);
    TEST_ASSERT((const GC_word *)(*slots[round][n]) == q);
    TEST_ASSERT(q[0] == (MAGIC ^ (GC_word)n));
    TEST_ASSERT(q[CHILD_WORDS - 1] == MAGIC + (GC_word)round);
  }
  if (n_lost > 0) {
    fprintf(stderr, "Round %d: %d of %d objects collected while reachable\n",
            round, n_lost, (int)N_LINKS);
    exit(1);
  }
}

int
main(void)
{
  int was_incremental;

  GC_INIT();
  if (GC_get_find_leak())
    printf("This test program is not designed for leak detection mode\n");

  /*
   * Only the collections below are done, as another collection between
   * the collection which makes the objects old and the pointer stores
   * could hide the bug being tested.
   */
  GC_set_disable_automatic_collection(1);
  (void)GC_expand_hp(
      (size_t)(N_ROUNDS * (N_BIG * BIG_BYTES + N_MEDIUM * MEDIUM_BYTES * 2)
               + ((size_t)4 << 20)));
  was_incremental = GC_is_incremental_mode();

  /* The objects allocated before the incremental mode is turned on. */
  alloc_objects(0);
  /* This collects if something is allocated (making the objects old). */
#ifndef NO_INCREMENTAL
  GC_enable_incremental();
#endif
  if (was_incremental)
    GC_gcollect();
  store_children(0);
  collect_partially();
  check_children(0);

  /* The objects allocated in the incremental mode. */
  alloc_objects(1);
  GC_gcollect();
  store_children(1);
  collect_partially();
  check_children(1);

  GC_gcollect();
  check_children(0);
  check_children(1);
  printf("Incremental mode: %s, heap size: %lu\n",
         GC_is_incremental_mode() ? "on" : "off",
         (unsigned long)GC_get_heap_size());
  printf("SUCCEEDED\n");
  return 0;
}
