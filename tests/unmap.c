/*
 * Exercise free-block coalescing with deliberately ordered free lists.
 * Include the collector to access private lists without exporting test APIs.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* Exercise region bookkeeping also on platforms using only madvise. */
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

/* Count actual header lookups, including those in list/mapping helpers. */
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

#include "../extra/gc.c"

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

static struct hblk *arena;
static hdr arena_header;
static struct hblk *piece[MAX_PIECES];
static size_t length[MAX_PIECES];
/* Zero: occupied separator; one: mapped free; two: unmapped free. */
static unsigned char state[MAX_PIECES];
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
      TEST_ASSERT(GC_install_counts(piece[i], length[i] * HBLKSIZE));
      memset(piece[i], 0x5a, length[i] * HBLKSIZE);
    }
    blocks += length[i];
  }
}

/* Permute list insertion order, independently of physical adjacency. */
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
      size_t j = GC_RAND_NEXT(&seed) % i;
      size_t tmp = order[i - 1];
      order[i - 1] = order[j];
      order[j] = tmp;
    }
  }
  for (i = 0; i < free_count; ++i) {
    struct hblk *h = piece[order[i]];
    GC_add_to_fl(h, HDR(h));
  }
  /* Set states only after all neighboring headers have been installed. */
  for (i = 0; i < piece_count; ++i) {
    if (state[i] == 2) {
      hdr *hhdr = HDR(piece[i]);
      GC_adjust_num_unmapped(piece[i], hhdr);
      GC_unmap((ptr_t)piece[i], hhdr->hb_sz);
      hhdr->hb_flags |= WAS_UNMAPPED;
    }
  }
}

/* Check independent physical and linked-list walks, including live payload. */
static size_t
check_layout(GC_bool coalesced)
{
  struct hblk *h;
  size_t i, nodes = 0, free_nodes = 0;
  word free_bytes = 0, unmapped_bytes = 0;
#  ifdef COUNT_UNMAPPED_REGIONS
  GC_signed_word regions = 0;
  GC_bool previous_unmapped = FALSE;
#  endif

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
      for (i = 0; i < blocks; ++i) {
        const unsigned char *p = (const unsigned char *)(h + i);
        TEST_ASSERT(p[0] == 0x5a && p[HBLKSIZE - 1] == 0x5a);
      }
    }
    if (!IS_MAPPED(hhdr)) {
      word start = (ADDR(h) + GC_page_size - 1) & ~(GC_page_size - 1);
      word end = (ADDR(h) + hhdr->hb_sz) & ~(GC_page_size - 1);
      if (end > start)
        unmapped_bytes += end - start;
    }
#  ifdef COUNT_UNMAPPED_REGIONS
    if (!IS_MAPPED(hhdr) && !previous_unmapped)
      ++regions;
    previous_unmapped = !IS_MAPPED(hhdr);
#  endif
    h += blocks;
  }
  for (i = 0; i <= N_HBLK_FLS; ++i) {
    struct hblk *prev = NULL;
    word bytes = 0;

    for (h = GC_hblkfreelist[i]; h != NULL; h = HDR(h)->hb_next) {
      hdr *hhdr = HDR(h);
      TEST_ASSERT(++nodes <= free_nodes); /* Also bounds cycles. */
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
#  ifdef COUNT_UNMAPPED_REGIONS
  if (regions != GC_num_unmapped_regions) {
    fprintf(stderr,
            "page=%lu coalesced=%d regions=%ld actual=%ld pieces=%lu\n",
            (unsigned long)GC_page_size, (int)coalesced, (long)regions,
            (long)GC_num_unmapped_regions, (unsigned long)piece_count);
    for (i = 0; i < piece_count; ++i)
      fprintf(stderr, "piece %lu: %lu blocks, state %u\n", (unsigned long)i,
              (unsigned long)length[i], (unsigned)state[i]);
  }
  TEST_ASSERT(regions == GC_num_unmapped_regions);
#  endif
  return free_nodes;
}

/* A virtual flag flip checks both adjacent ends without touching pages. */
static void
check_region_deltas(void)
{
#  ifdef COUNT_UNMAPPED_REGIONS
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
#  endif
}

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
    if (HBLK_IS_FREE(hhdr))
      GC_remove_from_fl(hhdr);
    else
      GC_remove_counts(h, hhdr->hb_sz);
    if (h != arena)
      GC_remove_header(h);
    h += blocks;
  }
  TEST_ASSERT(GC_unmapped_bytes == 0);
#  ifdef COUNT_UNMAPPED_REGIONS
  TEST_ASSERT(GC_num_unmapped_regions == 0);
#  endif
  GC_large_free_bytes = 0;
  *HDR(arena) = arena_header;
  TEST_ASSERT(GC_install_counts(arena, ARENA_BLOCKS * HBLKSIZE));
  piece_count = 0;
}

static void
run_layout(unsigned permutation)
{
  size_t before, after, work;
  GC_bool merged;

  make_layout();
  insert_layout(permutation);
  before = check_layout(FALSE);
  check_region_deltas();
  header_lookups = 0;
  merged = GC_merge_unmapped();
  work = header_lookups;
  after = check_layout(TRUE);
  TEST_ASSERT(merged == (after < before));
  if (before > 3000 || work > 64 * (before + 1))
    fprintf(stderr, "%lu lookups for %lu free blocks\n", (unsigned long)work,
            (unsigned long)before);
  TEST_ASSERT(work <= 64 * (before + 1));
  TEST_ASSERT(!GC_merge_unmapped());
  (void)check_layout(TRUE);
  clear_layout();
}

static void
test_layouts(void)
{
  unsigned a, b, permutation;
  size_t i, j;

  for (permutation = 0; permutation < 3; ++permutation) {
    append_piece(1, 0);
    run_layout(permutation); /* Empty free lists. */
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
    /* Same-bucket and cross-bucket cursor removal and repeated growth. */
    for (i = 0; i < 64; ++i) {
      static const size_t sizes[]
          = { 1, 2, 3, 31, 32, 33, 39, 40, 47, 63, 255, 256 };
      append_piece(1, 0);
      for (j = 0; j < 40; ++j) {
        size_t count = sizeof(sizes) / sizeof(sizes[0]);
        size_t size = sizes[j < count ? j : GC_RAND_NEXT(&seed) % count];
        unsigned char which = (unsigned char)(GC_RAND_NEXT(&seed) % 3);
        append_piece(size, which);
      }
      run_layout(permutation);
    }
  }

  /*
   * Reverse insertion leaves isolated blocks ahead of mergeable pairs in
   * bucket 1.  Restarting after each pair revisits the long isolated prefix.
   * The lookup bound above rejects that quadratic traversal deterministically.
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
  word saved_bytes[N_HBLK_FLS + 1], saved_free;
  word saved_page_size;

  GC_set_markers_count(1);
  GC_INIT();
  LOCK();
  GC_unmap_threshold = 0;
  UNLOCK();
  arena = (struct hblk *)GC_malloc_uncollectable(ARENA_BLOCKS * HBLKSIZE
                                                 - EXTRA_BYTES);
  TEST_ASSERT(arena != NULL);
  LOCK();
  TEST_ASSERT(HBLKPTR(arena) == arena);
  TEST_ASSERT(HDR(arena)->hb_sz == ARENA_BLOCKS * HBLKSIZE);
  TEST_ASSERT(GC_unmapped_bytes == 0);
  arena_header = *HDR(arena);
  memcpy(saved_lists, GC_hblkfreelist, sizeof(saved_lists));
  memcpy(saved_bytes, GC_free_bytes, sizeof(saved_bytes));
  saved_free = GC_large_free_bytes;
  memset(GC_hblkfreelist, 0, sizeof(GC_hblkfreelist));
  memset(GC_free_bytes, 0, sizeof(GC_free_bytes));
  GC_large_free_bytes = 0;
  test_layouts();
  /* Exercise partial-page gaps using a multiple of the actual OS page size. */
  saved_page_size = GC_page_size;
  if (GC_page_size <= 4 * HBLKSIZE && (4 * HBLKSIZE) % GC_page_size == 0) {
    GC_page_size = 4 * HBLKSIZE;
    test_layouts();
    GC_page_size = saved_page_size;
  }
  memcpy(GC_hblkfreelist, saved_lists, sizeof(saved_lists));
  memcpy(GC_free_bytes, saved_bytes, sizeof(saved_bytes));
  GC_large_free_bytes = saved_free;
  UNLOCK();
  GC_free(arena);
  GC_gcollect_and_unmap();
  arena = (struct hblk *)GC_malloc_uncollectable(ARENA_BLOCKS * HBLKSIZE
                                                 - EXTRA_BYTES);
  TEST_ASSERT(arena != NULL);
  memset(arena, 0x3c, ARENA_BLOCKS * HBLKSIZE - EXTRA_BYTES);
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
