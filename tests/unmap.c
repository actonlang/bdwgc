/*
 * Copyright (c) 2026 Kristian Larsson
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
 * Check the coalescing of free blocks (`GC_merge_unmapped()`) and
 * the counting of unmapped regions for deliberately laid out blocks and
 * ordered free lists.  This test includes the collector source to access
 * the free lists and the block headers, and to count the header lookups.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* Check the region counting also on the platforms using only `madvise`. */
#ifndef COUNT_UNMAPPED_REGIONS
#  define COUNT_UNMAPPED_REGIONS
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

#ifdef USE_MUNMAP

/* Count the header lookups, including those in the free-list helpers. */
static size_t header_lookups;

static hdr *
test_get_hdr(const void *p)
{
  hdr *result;

  GET_HDR(p, result);
  ++header_lookups;
  return result;
}

#  undef GET_HDR
#  define GET_HDR(p, hhdr) (void)((hhdr) = test_get_hdr(p))
#endif

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

#ifdef USE_MUNMAP

#  define TEST_ASSERT(e)                                                      \
    do {                                                                      \
      if (!(e)) {                                                             \
        fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
                #e);                                                          \
        exit(1);                                                              \
      }                                                                       \
    } while (0)

#  define ARENA_BLOCKS 8192
#  define MAX_PIECES ARENA_BLOCKS

/* The value written to the ends of the occupied pieces. */
#  define MARK_BYTE 0x5a

/*
 * The arena is an object which is split into pieces of the given lengths
 * (in blocks) and states.  The last piece is occupied, it fills the rest
 * of the arena.
 */
static struct hblk *arena;
static hdr arena_header;
static struct hblk *piece[MAX_PIECES];
static size_t length[MAX_PIECES];
/* Zero: occupied; one: mapped free block; two: unmapped free block. */
static unsigned char state[MAX_PIECES];
/* The indices of the free pieces, in the order of adding to free lists. */
static size_t order[MAX_PIECES];
static size_t piece_count;
static size_t free_count;
static GC_RAND_STATE_T seed;

static void
append_piece(size_t blocks, unsigned char which)
{
  TEST_ASSERT(piece_count < MAX_PIECES);
  length[piece_count] = blocks;
  state[piece_count++] = which;
}

/*
 * Return one (selected by `i`, from 0 to 3) of the first and the last
 * bytes of the first and the last blocks of an occupied piece.  Only the
 * pages of these blocks may be shared with the neighboring free blocks
 * (which are unmapped and remapped).
 */
static unsigned char *
piece_end_byte(struct hblk *h, size_t blocks, int i)
{
  unsigned char *p = (unsigned char *)(i < 2 ? h : h + blocks - 1);

  return (i & 1) != 0 ? p + HBLKSIZE - 1 : p;
}

static void
make_layout(void)
{
  size_t i, blocks = 0;

  GC_remove_counts(arena, ARENA_BLOCKS * HBLKSIZE);
  for (i = 0; i < piece_count; ++i)
    blocks += length[i];
  TEST_ASSERT(blocks < ARENA_BLOCKS);
  append_piece(ARENA_BLOCKS - blocks, 0);
  blocks = 0;
  free_count = 0;
  for (i = 0; i < piece_count; ++i) {
    hdr *hhdr;

    piece[i] = arena + blocks;
    hhdr = i == 0 ? HDR(arena) : GC_install_header(piece[i]);
    TEST_ASSERT(hhdr != NULL);
    TEST_ASSERT(
        setup_header(hhdr, piece[i], length[i] * HBLKSIZE, PTRFREE, 0));
    if (state[i] != 0) {
      hhdr->hb_flags |= FREE_BLK;
      GC_large_free_bytes += length[i] * HBLKSIZE;
      order[free_count++] = i;
    } else {
      int j;

      TEST_ASSERT(GC_install_counts(piece[i], length[i] * HBLKSIZE));
      for (j = 0; j < 4; ++j)
        *piece_end_byte(piece[i], length[i], j) = MARK_BYTE;
    }
    blocks += length[i];
  }
}

/*
 * Add the free pieces to the free lists (each one is added at the head
 * of its list), in the order given by `order` permuted (independently of
 * the addresses) as follows: zero means no change, one means reversal,
 * two means a pseudo-random shuffle.  Then unmap the pieces which should
 * be unmapped.
 */
static void
insert_layout(unsigned permutation)
{
  size_t i;

  if (permutation == 1) {
    for (i = 0; i < free_count / 2; ++i) {
      size_t tmp = order[i];

      order[i] = order[free_count - 1 - i];
      order[free_count - 1 - i] = tmp;
    }
  } else if (permutation == 2) {
    for (i = free_count; i > 1; --i) {
      size_t j = (size_t)GC_RAND_NEXT(&seed) % i;
      size_t tmp = order[i - 1];

      order[i - 1] = order[j];
      order[j] = tmp;
    }
  }
  for (i = 0; i < free_count; ++i) {
    struct hblk *h = piece[order[i]];

    GC_add_to_fl(h, HDR(h));
  }
  /* Unmap only after all the neighboring headers have been installed. */
  for (i = 0; i < piece_count; ++i) {
    if (state[i] == 2) {
      hdr *hhdr = HDR(piece[i]);

      GC_adjust_num_unmapped(piece[i], hhdr);
      GC_unmap((ptr_t)piece[i], hhdr->hb_sz);
      hhdr->hb_flags |= WAS_UNMAPPED;
    }
  }
}

/*
 * Check the blocks (walking them by address) against the free lists, the
 * free and unmapped bytes counters, and the number of unmapped regions;
 * check the occupied pieces keep their contents.  If `coalesced`, then
 * also check no free block is followed by a free one.  Return the number
 * of free blocks.
 */
static size_t
check_layout(GC_bool coalesced)
{
  struct hblk *h;
  size_t i, nodes = 0, free_nodes = 0;
  word free_bytes = 0, unmapped_bytes = 0;
  GC_signed_word regions = 0;
  GC_bool previous_unmapped = FALSE;

  for (h = arena; h < arena + ARENA_BLOCKS;) {
    hdr *hhdr = HDR(h);
    size_t blocks;

    TEST_ASSERT(!IS_FORWARDING_ADDR_OR_NIL(hhdr));
    TEST_ASSERT(hhdr->hb_sz > 0 && modHBLKSZ(hhdr->hb_sz) == 0);
    blocks = divHBLKSZ(hhdr->hb_sz);
    TEST_ASSERT(blocks <= (size_t)(arena + ARENA_BLOCKS - h));
    if (HBLK_IS_FREE(hhdr)) {
      ++free_nodes;
      free_bytes += hhdr->hb_sz;
      if (coalesced && h + blocks < arena + ARENA_BLOCKS)
        TEST_ASSERT(!HBLK_IS_FREE(HDR(h + blocks)));
    } else {
      int j;

      for (j = 0; j < 4; ++j)
        TEST_ASSERT(MARK_BYTE == *piece_end_byte(h, blocks, j));
    }
    if (!IS_MAPPED(hhdr)) {
      /* Only the pages entirely within the block are unmapped. */
      word start = (ADDR(h) + GC_page_size - 1) & ~(GC_page_size - 1);
      word end = (ADDR(h) + hhdr->hb_sz) & ~(GC_page_size - 1);

      if (end > start)
        unmapped_bytes += end - start;
      if (!previous_unmapped)
        ++regions;
    }
    previous_unmapped = !IS_MAPPED(hhdr);
    h += blocks;
  }
  for (i = 0; i <= N_HBLK_FLS; ++i) {
    struct hblk *prev = NULL;
    word bytes = 0;

    for (h = GC_hblkfreelist[i]; h != NULL; h = HDR(h)->hb_next) {
      const hdr *hhdr = HDR(h);

      /* This also bounds the walk if the list is cyclic. */
      TEST_ASSERT(++nodes <= free_nodes);
      TEST_ASSERT(h >= arena && h < arena + ARENA_BLOCKS);
      TEST_ASSERT(HBLK_IS_FREE(hhdr));
      TEST_ASSERT(hhdr->hb_prev == prev);
      TEST_ASSERT(GC_hblk_fl_from_blocks(divHBLKSZ(hhdr->hb_sz)) == i);
      bytes += hhdr->hb_sz;
      prev = h;
    }
    TEST_ASSERT(bytes == GC_free_bytes[i]);
  }
  TEST_ASSERT(nodes == free_nodes);
  TEST_ASSERT(free_bytes == GC_large_free_bytes);
  TEST_ASSERT(unmapped_bytes == GC_unmapped_bytes);
  if (regions != GC_num_unmapped_regions) {
    fprintf(stderr,
            "Page size: %lu, coalesced: %d, unmapped regions: %ld"
            " (counted: %ld)\n",
            (unsigned long)GC_page_size, (int)coalesced, (long)regions,
            (long)GC_num_unmapped_regions);
    for (i = 0; i < piece_count; ++i)
      fprintf(stderr, "Piece %lu: %lu blocks, state %u\n", (unsigned long)i,
              (unsigned long)length[i], (unsigned)state[i]);
    exit(1);
  }
  return free_nodes;
}

/*
 * Check the change of the number of unmapped regions, as computed for
 * a block about to be unmapped or remapped, for each free block (without
 * changing the block state).  Both the neighbors matter.
 */
static void
check_region_deltas(void)
{
  struct hblk *h;

  if (free_count > 100)
    return;
  for (h = arena; h < arena + ARENA_BLOCKS; h += divHBLKSZ(HDR(h)->hb_sz)) {
    struct hblk *p;
    GC_signed_word regions = 0;
    GC_bool previous_unmapped = FALSE;

    if (!HBLK_IS_FREE(HDR(h)))
      continue;
    for (p = arena; p < arena + ARENA_BLOCKS; p += divHBLKSZ(HDR(p)->hb_sz)) {
      GC_bool unmapped = !IS_MAPPED(HDR(p));

      if (p == h)
        unmapped = !unmapped;
      if (unmapped && !previous_unmapped)
        ++regions;
      previous_unmapped = unmapped;
    }
    TEST_ASSERT(calc_num_unmapped_regions_delta(h, HDR(h))
                == regions - GC_num_unmapped_regions);
  }
}

/* Remap and remove the pieces, and restore the arena object. */
static void
clear_layout(void)
{
  struct hblk *h;

  for (h = arena; h < arena + ARENA_BLOCKS;) {
    hdr *hhdr = HDR(h);
    size_t blocks = divHBLKSZ(hhdr->hb_sz);

    if (!IS_MAPPED(hhdr)) {
      GC_adjust_num_unmapped(h, hhdr);
      GC_remap((ptr_t)h, hhdr->hb_sz);
      hhdr->hb_flags &= (unsigned char)~WAS_UNMAPPED;
    }
    if (HBLK_IS_FREE(hhdr)) {
      GC_remove_from_fl(hhdr);
    } else {
      GC_remove_counts(h, hhdr->hb_sz);
    }
    if (h != arena)
      GC_remove_header(h);
    h += blocks;
  }
  TEST_ASSERT(0 == GC_unmapped_bytes);
  TEST_ASSERT(0 == GC_num_unmapped_regions);
  GC_large_free_bytes = 0;
  *HDR(arena) = arena_header;
  TEST_ASSERT(GC_install_counts(arena, ARENA_BLOCKS * HBLKSIZE));
  piece_count = 0;
}

static void
run_layout(unsigned permutation)
{
  size_t before, after, lookups;
  GC_bool merged;

  make_layout();
  insert_layout(permutation);
  before = check_layout(FALSE);
  check_region_deltas();
  header_lookups = 0;
  merged = GC_merge_unmapped();
  lookups = header_lookups;
  after = check_layout(TRUE);
  TEST_ASSERT(merged == (after < before));

  /*
   * Each visit of a free block takes two header lookups, and a block is
   * visited once more only after a merge.  A merge takes about a dozen
   * lookups (to update the free lists and the number of unmapped
   * regions).  The number of merges is less than the number of free
   * blocks, so 64 lookups per free block is a generous bound, while
   * restarting the scan of a free list after each merge needs a number
   * of lookups which grows quadratically with the length of the list.
   */
  if (lookups > 64 * (before + 1)) {
    fprintf(stderr, "%lu header lookups to coalesce %lu free blocks\n",
            (unsigned long)lookups, (unsigned long)before);
    exit(1);
  }
  TEST_ASSERT(!GC_merge_unmapped());
  (void)check_layout(TRUE);
  clear_layout();
}

/*
 * The mapping of a merged block depends on the order of the merges (the
 * bigger block of a pair decides).  Check the order is the same as when
 * the scan of a free list was restarted after each merge.  Blocks W, h,
 * Y and Z are adjacent (in this order), W and Z are unmapped, the list of
 * one-block free blocks is [h, W, Y].  Then h is merged with Y (and moves
 * to the list of two-block ones), then W is merged with the result and
 * the latter is remapped, then the result is merged with Z which is
 * remapped too.  (Merging h with Y and then with Z would leave all the
 * blocks unmapped.)
 */
static void
test_merge_order(void)
{
  hdr *hhdr;

  append_piece(1, 0);
  append_piece(1, 2); /*< W */
  append_piece(1, 1); /*< h */
  append_piece(1, 1); /*< Y */
  append_piece(2, 2); /*< Z */
  append_piece(1, 0);
  make_layout();
  TEST_ASSERT(4 == free_count);
  order[0] = 3;
  order[1] = 1;
  order[2] = 2;
  order[3] = 4;
  insert_layout(0);
  TEST_ASSERT(4 == check_layout(FALSE));
  TEST_ASSERT(GC_merge_unmapped());
  TEST_ASSERT(1 == check_layout(TRUE));
  hhdr = HDR(piece[1]);
  TEST_ASSERT(HBLK_IS_FREE(hhdr) && hhdr->hb_sz == 5 * HBLKSIZE);
  TEST_ASSERT(IS_MAPPED(hhdr));
  clear_layout();
}

static void
test_layouts(void)
{
  unsigned a, b, permutation;
  size_t i, j;

  test_merge_order();
  for (permutation = 0; permutation < 3; ++permutation) {
    /* Empty free lists. */
    append_piece(1, 0);
    run_layout(permutation);

    append_piece(1, 0);
    append_piece(1, 2);
    run_layout(permutation);
    for (a = 1; a <= 2; ++a) {
      for (b = 1; b <= 2; ++b) {
        for (i = 1; i <= 3; ++i) {
          append_piece(1, 0);
          append_piece(i, (unsigned char)a);
          append_piece(2, (unsigned char)b);
          run_layout(permutation);
        }
      }
    }

    /*
     * Sizes around the free-list boundaries, so that a merged block may
     * stay in its list or move to another one, and a block absorbed by
     * a merge may be the next one in the list being scanned.
     */
    for (i = 0; i < 64; ++i) {
      static const size_t sizes[]
          = { 1, 2, 3, 31, 32, 33, 39, 40, 47, 63, 255, 256 };
      size_t count = sizeof(sizes) / sizeof(sizes[0]);

      append_piece(1, 0);
      for (j = 0; j < 40; ++j) {
        size_t size
            = sizes[j < count ? j : (size_t)GC_RAND_NEXT(&seed) % count];
        unsigned char which = (unsigned char)(GC_RAND_NEXT(&seed) % 3);

        append_piece(size, which);
      }
      run_layout(permutation);
    }
  }

  /*
   * The reversed insertion puts 1024 isolated free blocks ahead of 1024
   * mergeable pairs in the list of one-block free blocks.  Restarting the
   * scan after each merge would revisit the isolated blocks each time.
   */
  append_piece(1, 0);
  for (i = 0; i < 1024; ++i) {
    append_piece(1, 1);
    append_piece(1, 0);
  }
  for (i = 0; i < 1024; ++i) {
    append_piece(1, 1);
    append_piece(1, 2);
    append_piece(1, 0);
  }
  run_layout(1);
}

int
main(void)
{
  struct hblk *saved_lists[N_HBLK_FLS + 1];
  word saved_bytes[N_HBLK_FLS + 1], saved_free_bytes;
  size_t real_page_size, i;
  unsigned saved_unmap_threshold;

  GC_set_markers_count(1);
  GC_INIT();
  /* The arena should not be limited by `GC_MAXIMUM_HEAP_SIZE` variable. */
  GC_set_max_heap_size(0);
  LOCK();
  /* Do not unmap any blocks except for those of the test. */
  saved_unmap_threshold = GC_unmap_threshold;
  GC_unmap_threshold = 0;
  UNLOCK();
  arena = (struct hblk *)GC_malloc_uncollectable(ARENA_BLOCKS * HBLKSIZE
                                                 - EXTRA_BYTES);
  TEST_ASSERT(arena != NULL);
  LOCK();
  TEST_ASSERT(HBLKPTR(arena) == arena);
  TEST_ASSERT(HDR(arena)->hb_sz == ARENA_BLOCKS * HBLKSIZE);
  TEST_ASSERT(0 == GC_unmapped_bytes);
  TEST_ASSERT(0 == GC_num_unmapped_regions);
  arena_header = *HDR(arena);

  /* Replace the free lists with empty ones for the test. */
  BCOPY(GC_hblkfreelist, saved_lists, sizeof(saved_lists));
  BCOPY(GC_free_bytes, saved_bytes, sizeof(saved_bytes));
  saved_free_bytes = GC_large_free_bytes;
  BZERO(GC_hblkfreelist, sizeof(GC_hblkfreelist));
  BZERO(GC_free_bytes, sizeof(GC_free_bytes));
  GC_large_free_bytes = 0;
  test_layouts();

  /*
   * Use a bigger page size (a multiple of the actual one), so that also
   * the partial pages at the block ends are covered.
   */
  real_page_size = GC_page_size;
  if (GC_page_size < 4 * HBLKSIZE && (4 * HBLKSIZE) % GC_page_size == 0) {
    GC_page_size = 4 * HBLKSIZE;
    test_layouts();
    GC_page_size = real_page_size;
  }
  BCOPY(saved_lists, GC_hblkfreelist, sizeof(saved_lists));
  BCOPY(saved_bytes, GC_free_bytes, sizeof(saved_bytes));
  GC_large_free_bytes = saved_free_bytes;
  GC_unmap_threshold = saved_unmap_threshold;
  UNLOCK();

  /* Check the arena memory is usable after unmapping and remapping. */
  GC_free(arena);
  GC_gcollect_and_unmap();
  arena = (struct hblk *)GC_malloc_uncollectable(ARENA_BLOCKS * HBLKSIZE
                                                 - EXTRA_BYTES);
  TEST_ASSERT(arena != NULL);
  for (i = 0; i < ARENA_BLOCKS; ++i)
    *(volatile unsigned char *)(arena + i) = MARK_BYTE;
  GC_free(arena);
  GC_gcollect();
  printf("SUCCEEDED\n");
  return 0;
}

#else

int
main(void)
{
  printf("Unmapping is disabled; skipping free-block coalescing test\n");
  return 0;
}

#endif /* !USE_MUNMAP */
