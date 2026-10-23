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
 * Check the memory limit of the process.  On Linux, check the cgroup
 * memory limit found in synthetic lists of the cgroups of a process and
 * synthetic cgroup trees: a limit of the cgroup of the process, a smaller
 * one above it, no limit ("max"), the root cgroup of a namespace, a path
 * missing under the mount point, the v1 memory controller alone, in a
 * list of controllers and together with v2 (where v1 is used), the v1
 * value for no limit, malformed and huge numbers, a trailing slash, a
 * limit above the mount point (which is not read), a list without the
 * memory controller, a missing list and a path too long.  Then, after
 * the collector initialization, check that the size of the physical
 * memory is found on Linux, macOS and Windows, and that it matches
 * `/proc/meminfo` pseudo-file on Linux and `hw.memsize` on macOS, that
 * the memory limit is the smaller one of it and of the cgroup limit, and
 * that `GC_set_memory_limit()` lowers the limit and cannot raise it.
 * Then check the heap sizing by the memory limit, with an allocation
 * budget of 200% and a memory limit of 64 MiB (the heap ceiling is
 * 48 MiB): as the live data grows step by step from 2 MiB to 52 MiB,
 * after each step the allocation volume between collections is 200% of
 * the live data, at most half of the distance to the ceiling, and at
 * least 10% of the live data, and the heap target is the live data plus
 * the volume.  Allocating garbage of 6 volumes in small objects causes
 * a collection after about the volume plus the roots and the stacks
 * (which the live data counts, but which are not in the heap), 13
 * collections at most, and the heap stays below the target plus a slack.
 * With the live data above the ceiling, garbage of 6 volumes in large
 * objects of 256 KiB to 4 MiB causes 13 collections at most, and the
 * heap stays below twice the target.  With the heap above
 * the target, a request of more bytes than the free blocks hold grows
 * the heap without a collection until half of the volume, but at least
 * 10% of the live data, has been allocated since the latest collection
 * (checked midway between the two, with the volume below and above 20%
 * of the live data), and causes one collection after 3/4 of the volume.
 * A heap growth reaches the target, and is 10% of the live data at
 * least.  Without the ceiling, or with the collections disabled or in
 * the incremental mode, there is no heap target.  This test includes the
 * collector source to call its static functions.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
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

#ifdef DARWIN
#  include <sys/sysctl.h>
#endif

#define TEST_ASSERT(e)                                                      \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define MiB ((word)1 << 20)

#ifdef LINUX

/*
 * The root of the synthetic trees.  The list of the cgroups is the file
 * "cgroup" in it, the mount point is the directory "fs" in it.
 */
static char tree_root[256];

/* The files and directories created in the root, to remove them. */
#  define MAX_TREE_PATHS 32
static char tree_paths[MAX_TREE_PATHS][CGROUP_PATH_SZ];
static int n_tree_paths = 0;

/*
 * Put the concatenation of the root and the first `rel_len` characters
 * of `rel` into the next entry of `tree_paths`, and return that entry.
 * The caller counts the entry if the path is created.
 */
static char *
next_tree_path(const char *rel, size_t rel_len)
{
  char *path;
  int len;

  TEST_ASSERT(n_tree_paths < MAX_TREE_PATHS);
  path = tree_paths[n_tree_paths];
  len = snprintf(path, sizeof(tree_paths[0]), "%s%.*s", tree_root,
                 (int)rel_len, rel);
  TEST_ASSERT(len > 0 && (size_t)len < sizeof(tree_paths[0]));
  return path;
}

/*
 * Add a file of the given contents and the path `rel` (which starts with
 * '/') relative to the root to the synthetic tree, creating the missing
 * directories.
 */
static void
add_tree_file(const char *rel, const char *contents)
{
  const char *p;
  const char *path;
  FILE *f;

  for (p = strchr(rel + 1, '/'); p != NULL; p = strchr(p + 1, '/')) {
    path = next_tree_path(rel, (size_t)(p - rel));
    if (mkdir(path, 0700) == 0) {
      n_tree_paths++;
    } else {
      TEST_ASSERT(EEXIST == errno);
    }
  }
  path = next_tree_path(rel, strlen(rel));
  f = fopen(path, "w");
  TEST_ASSERT(f != NULL);
  n_tree_paths++;
  TEST_ASSERT(fputs(contents, f) >= 0);
  TEST_ASSERT(fclose(f) == 0);
}

/*
 * Check the cgroup memory limit found in the synthetic tree, and remove
 * the tree.
 */
static void
check_tree(const char *name, word expected)
{
  char proc_cgroup[sizeof(tree_root) + 16];
  char cgroup_fs[sizeof(tree_root) + 16];
  word limit;
  IF_CANCEL(int cancel_state;)

  TEST_ASSERT(
      snprintf(proc_cgroup, sizeof(proc_cgroup), "%s/cgroup", tree_root) > 0);
  TEST_ASSERT(snprintf(cgroup_fs, sizeof(cgroup_fs), "%s/fs", tree_root) > 0);
  /* The file reading of the collector expects this. */
  DISABLE_CANCEL(cancel_state);
  limit = GC_cgroup_memory_limit(proc_cgroup, cgroup_fs);
  RESTORE_CANCEL(cancel_state);
  if (limit != expected) {
    fprintf(stderr, "Synthetic cgroup tree (%s): limit %lu (%lu expected)\n",
            name, (unsigned long)limit, (unsigned long)expected);
    exit(1);
  }
  while (n_tree_paths > 0) {
    const char *path = tree_paths[--n_tree_paths];

    TEST_ASSERT(remove(path) == 0);
  }
}

static void
test_cgroup_trees(void)
{
  /* `/tmp` does not exist everywhere, e.g. on Android. */
  const char *dir = getenv("TMPDIR");
  static char long_list[CGROUP_PATH_SZ + 32];
  word v1_unlimited;
  size_t i;
  int len;

  if (NULL == dir || '\0' == *dir)
    dir = "/tmp";
  len = snprintf(tree_root, sizeof(tree_root), "%s/memlimittest.XXXXXX", dir);
  TEST_ASSERT(len > 0 && (size_t)len < sizeof(tree_root));
  TEST_ASSERT(mkdtemp(tree_root) != NULL);

  /*
   * cgroup v2: the limit of the cgroup of the process, and a smaller
   * one above it.
   */
  add_tree_file("/cgroup", "0::/a/b\n");
  add_tree_file("/fs/a/b/memory.max", "1048576\n");
  add_tree_file("/fs/a/memory.max", "max\n");
  check_tree("leaf limit", MiB);
  add_tree_file("/cgroup", "0::/a/b\n");
  add_tree_file("/fs/a/b/memory.max", "8388608\n");
  add_tree_file("/fs/a/memory.max", "4194304\n");
  add_tree_file("/fs/memory.max", "max\n");
  check_tree("smaller parent limit", 4 * MiB);

  /* No limit at any level. */
  add_tree_file("/cgroup", "0::/a/b\n");
  add_tree_file("/fs/a/b/memory.max", "max\n");
  add_tree_file("/fs/a/memory.max", "max\n");
  add_tree_file("/fs/memory.max", "max\n");
  check_tree("no limit", 0);

  /*
   * The root cgroup of a namespace, whose limit is in the mount point,
   * and a path of the host which does not exist in a container.
   */
  add_tree_file("/cgroup", "0::/\n");
  add_tree_file("/fs/memory.max", "2097152\n");
  check_tree("namespace root", 2 * MiB);
  add_tree_file("/cgroup", "0::/x/y/z\n");
  add_tree_file("/fs/memory.max", "3145728\n");
  check_tree("missing path", 3 * MiB);

  /* A limit above the mount point is not read. */
  add_tree_file("/cgroup", "0::/a\n");
  add_tree_file("/fs/a/memory.max", "5242880\n");
  add_tree_file("/memory.max", "1024\n");
  check_tree("limit above the mount point", 5 * MiB);

  /* A trailing slash. */
  add_tree_file("/cgroup", "0::/a/\n");
  add_tree_file("/fs/a/memory.max", "6291456\n");
  check_tree("trailing slash", 6 * MiB);

  /*
   * cgroup v1: the memory controller alone, in a list of controllers,
   * and the value for no limit (the largest multiple of the page size
   * below 2**63), saturated on 32-bit targets.
   */
  add_tree_file("/cgroup", "5:memory:/c\n"
                           "1:name=systemd:/c\n");
  add_tree_file("/fs/memory/c/memory.limit_in_bytes", "6291456\n");
  check_tree("v1", 6 * MiB);
  add_tree_file("/cgroup", "7:cpu,memory,cpuset:/d\n");
  add_tree_file("/fs/memory/d/memory.limit_in_bytes", "5242880\n");
  add_tree_file("/fs/memory/memory.limit_in_bytes", "9223372036854771712\n");
  check_tree("v1 controller list", 5 * MiB);
  v1_unlimited
      = CPP_WORDSZ >= 64 ? ((word)1 << (CPP_WORDSZ - 1)) - 4096 : GC_WORD_MAX;
  add_tree_file("/cgroup", "5:memory:/\n");
  add_tree_file("/fs/memory/memory.limit_in_bytes", "9223372036854771712\n");
  check_tree("v1 no limit", v1_unlimited);

  /*
   * Both v1 and v2 (the memory controller is attached to v1), in either
   * order.
   */
  add_tree_file("/cgroup", "0::/a\n"
                           "4:memory:/c\n");
  add_tree_file("/fs/a/memory.max", "1048576\n");
  add_tree_file("/fs/memory/c/memory.limit_in_bytes", "7340032\n");
  check_tree("v2 and v1", 7 * MiB);
  add_tree_file("/cgroup", "4:memory:/c\n"
                           "0::/a\n");
  add_tree_file("/fs/a/memory.max", "1048576\n");
  add_tree_file("/fs/memory/c/memory.limit_in_bytes", "7340032\n");
  check_tree("v1 and v2", 7 * MiB);

  /* Malformed numbers are skipped, a huge one is saturated. */
  add_tree_file("/cgroup", "0::/a/b\n");
  add_tree_file("/fs/a/b/memory.max", "12x\n");
  add_tree_file("/fs/a/memory.max", " 1024\n");
  add_tree_file("/fs/memory.max", "8388608\n");
  check_tree("malformed numbers", 8 * MiB);
  add_tree_file("/cgroup", "0::/a\n");
  add_tree_file("/fs/a/memory.max", "\n");
  add_tree_file("/fs/memory.max", "123456789012345678901234567890\n");
  check_tree("huge number", GC_WORD_MAX);

  /* Neither the v1 memory controller nor v2 is listed. */
  add_tree_file("/cgroup", "3:cpu:/a\n"
                           "2:memory2:/a\n");
  add_tree_file("/fs/a/memory.max", "1048576\n");
  add_tree_file("/fs/memory/a/memory.limit_in_bytes", "1048576\n");
  check_tree("no memory controller", 0);

  /* The list of the cgroups is missing or empty. */
  add_tree_file("/fs/memory.max", "1048576\n");
  check_tree("missing list", 0);
  add_tree_file("/cgroup", "");
  add_tree_file("/fs/memory.max", "1048576\n");
  check_tree("empty list", 0);

  /* The path of the limit file would be longer than 1 KiB. */
  strcpy(long_list, "0::");
  for (i = strlen(long_list); i < CGROUP_PATH_SZ; i += 2)
    strcpy(&long_list[i], "/a");
  strcpy(&long_list[i], "\n");
  add_tree_file("/cgroup", long_list);
  add_tree_file("/fs/memory.max", "1048576\n");
  check_tree("path too long", 0);

  TEST_ASSERT(rmdir(tree_root) == 0);
}

/*
 * Return the size of the physical memory given by `MemTotal` line of
 * `/proc/meminfo` pseudo-file, in bytes, saturated at `GC_WORD_MAX` (as
 * the collector saturates it).
 */
static word
meminfo_total(void)
{
  char line[256];
  FILE *f = fopen("/proc/meminfo", "r");
  unsigned long kib = 0;

  TEST_ASSERT(f != NULL);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (sscanf(line, "MemTotal: %lu kB", &kib) == 1)
      break;
  }
  TEST_ASSERT(fclose(f) == 0);
  TEST_ASSERT(kib > 0);
  return (word)kib > GC_WORD_MAX >> 10 ? GC_WORD_MAX : (word)kib << 10;
}

#endif /* LINUX */

/*
 * Check the size of the physical memory and the memory limit found by
 * the collector initialization, and the setter of the memory limit.
 */
static void
test_memory_limit(void)
{
  word physical = GC_physical_memory_size();
  word cgroup = 0;
  word initial_limit, limit;

  GC_INIT();
#ifdef LINUX
  {
    IF_CANCEL(int cancel_state;)

    DISABLE_CANCEL(cancel_state);
    cgroup = GC_cgroup_memory_limit("/proc/self/cgroup", "/sys/fs/cgroup");
    RESTORE_CANCEL(cancel_state);
  }
#endif
  initial_limit = GC_get_memory_limit();
  GC_set_memory_limit(0);
  limit = GC_get_memory_limit();
  printf("Memory limit: %lu MiB (physical memory: %lu MiB,"
         " cgroup limit: %lu MiB, initially: %lu MiB)\n",
         (unsigned long)(limit >> 20), (unsigned long)(physical >> 20),
         (unsigned long)(cgroup >> 20), (unsigned long)(initial_limit >> 20));
  TEST_ASSERT(limit == min_known_limit(physical, cgroup));
#if defined(LINUX) || defined(DARWIN) || defined(MSWIN32)
  TEST_ASSERT(physical > 0);
#endif
#ifdef LINUX
  {
    word total = meminfo_total();
    word page_size = (word)sysconf(_SC_PAGESIZE);

    /* `sysconf()` rounds the size down to whole pages. */
    TEST_ASSERT(physical <= total && total - physical < page_size);
  }
#elif defined(DARWIN)
  {
    unsigned long long memsize = 0;
    size_t len = sizeof(memsize);

    TEST_ASSERT(sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0);
    TEST_ASSERT(physical == (word)memsize);
  }
#endif

  if (limit > 0) {
    /* The setter lowers the limit, and cannot raise it. */
    GC_set_memory_limit(limit / 2);
    TEST_ASSERT(GC_get_memory_limit() == limit / 2);
    GC_set_memory_limit(GC_WORD_MAX);
    TEST_ASSERT(GC_get_memory_limit() == limit);
  } else {
    /* Without a known limit, the value of the setter is the limit. */
    GC_set_memory_limit(64 * MiB);
    TEST_ASSERT(GC_get_memory_limit() == 64 * MiB);
  }
  GC_set_memory_limit(0);
  TEST_ASSERT(GC_get_memory_limit() == limit);
  if (getenv("GC_MEMORY_LIMIT") == NULL) {
    /* The macro is defined (to 0 by default) in the collector source. */
    TEST_ASSERT(initial_limit
                == min_known_limit(limit, (word)GC_MEMORY_LIMIT));
  }
}

/* The memory limit of the heap sizing checks (the ceiling is 48 MiB). */
#define TEST_MEMORY_LIMIT (64 * MiB)

/* The allocation budget percentage of the heap sizing checks. */
#define TEST_BUDGET_PERCENT 200

/*
 * The size of a node of the live list: 1 KiB, or, with blocks of 2 KiB or
 * less, the largest small object less the byte which the collector may
 * add at the end (two nodes per block).  The size of the live data, as
 * the collector counts it, is then about the heap space which the list
 * takes.  A node of 1 KiB in a block of 1 KiB would be a large object of
 * two blocks, of which the live data counts just over one.
 */
#define LIVE_NODE_BYTES \
  (MAXOBJBYTES > 1024 ? 1024 : MAXOBJBYTES - MAX_EXTRA_BYTES)

/* The size of a garbage object. */
#define GARBAGE_BYTES 128

/* The number of allocation volumes allocated in garbage per step. */
#define CYCLES_PER_STEP 6

struct node_s {
  struct node_s *next;
};

/* The live list, and its size in bytes. */
static struct node_s *volatile live_list;
static word live_list_bytes;

static void *
checked_malloc(size_t lb)
{
  void *p = GC_MALLOC(lb);

  if (NULL == p) {
    fprintf(stderr, "Out of memory\n");
    exit(69);
  }
  return p;
}

/* Grow the live list to `bytes` bytes. */
static void
grow_live_list(word bytes)
{
  while (live_list_bytes < bytes) {
    struct node_s *p = (struct node_s *)checked_malloc(LIVE_NODE_BYTES);

    p->next = live_list;
    live_list = p;
    live_list_bytes += LIVE_NODE_BYTES;
  }
}

/* Allocate `bytes` bytes in unreachable objects of `lb` bytes each. */
static void
alloc_garbage(word bytes, size_t lb)
{
  void *volatile p = NULL;
  word n;

  for (n = 0; n < bytes; n += lb)
    p = checked_malloc(lb);
  (void)p;
}

/*
 * Return the allocation volume between collections which the heap sizing
 * gives for `live` bytes of live data and the heap ceiling `ceiling`.
 */
static word
expected_volume(word live, word ceiling)
{
  word volume = percent_of(live, TEST_BUDGET_PERCENT);

  if (ceiling != 0) {
    word bound = ceiling > live ? percent_of(ceiling - live, 50) : 0;

    if (bound < percent_of(live, 10))
      bound = percent_of(live, 10);
    if (volume > bound)
      volume = bound;
  }
  if (GC_incremental)
    volume /= 2;
  return volume > min_bytes_allocd_minimum ? volume : min_bytes_allocd_minimum;
}

/*
 * Tell whether `a` and `b` differ by 4 KiB at most.  The size of the
 * stack in the live data is measured at each call (unless other threads
 * are running), thus it differs by the depth of the calls.
 */
static GC_bool
close_to(word a, word b)
{
  return a > b ? a - b <= 4096 : b - a <= 4096;
}

/* The state of the heap sizing read under the allocator lock. */
struct sizing_s {
  word live;
  word roots;
  word volume;
  word target;
  word heap;
};

static struct sizing_s
read_sizing(void)
{
  struct sizing_s r;

  LOCK();
  r.live = live_bytes();
  r.roots = total_stack_size() + GC_root_size;
  r.volume = min_bytes_allocd();
  r.target = heap_target();
  r.heap = GC_heapsize;
  UNLOCK();
  return r;
}

/*
 * Grow the live list to `step` bytes, collect, check the allocation
 * volume and the heap target, then allocate `CYCLES_PER_STEP` volumes in
 * objects of `lb` bytes, and check the number of collections and the
 * heap size (unless `strict` is false).  Small objects let the heap
 * exceed the target by 10% of the live data (the least heap growth),
 * `MINHINCR` blocks and 2 MiB.  Large objects (of `HBLKSIZE` bytes or
 * more) may be allocated with collections which do not free large
 * enough runs of free blocks, and then the heap grows, up to twice the
 * target.  The heap is not shrunk, thus it may exceed these bounds if it
 * was larger at the start (`initial_heap`).
 */
static void
check_step(word step, size_t lb, GC_bool strict, word initial_heap)
{
  struct sizing_s s;
  word slack, churn, heap, gc_no;
  unsigned long n;

  grow_live_list(step);
  GC_gcollect();
  s = read_sizing();
  TEST_ASSERT(close_to(s.volume, expected_volume(s.live, heap_ceiling())));
  TEST_ASSERT(GC_incremental ? 0 == s.target
                             : close_to(s.target, s.live + s.volume));
  slack = lb < HBLKSIZE
              ? percent_of(s.live, 10) + (word)MINHINCR * HBLKSIZE + 2 * MiB
              : s.target;
  churn = CYCLES_PER_STEP * s.volume;
  gc_no = GC_get_gc_no();
  alloc_garbage(churn, lb);
  n = (unsigned long)(GC_get_gc_no() - gc_no);
  heap = read_sizing().heap;
  printf("List %2lu MiB, objects of %7lu bytes: live %6lu KiB,"
         " volume %6lu KiB (%3lu%%), target %6lu KiB, heap %6lu KiB,"
         " %2lu collections\n",
         (unsigned long)(step >> 20), (unsigned long)lb,
         (unsigned long)(s.live >> 10), (unsigned long)(s.volume >> 10),
         (unsigned long)(s.volume / (s.live / 100)),
         (unsigned long)(s.target >> 10), (unsigned long)(heap >> 10), n);
  if (strict) {
    /*
     * A collection comes when the heap runs out of free blocks.  The
     * target counts the roots and the stacks, which are not in the
     * heap, thus a heap at the target holds about the volume plus them
     * free after a collection, or more if the heap exceeds the target.
     */
    TEST_ASSERT(n <= 2 * CYCLES_PER_STEP + 1);
    TEST_ASSERT((word)n + 1 >= churn / (s.volume + s.roots + slack));
    TEST_ASSERT(heap <= initial_heap || heap <= s.target + slack);
  }
}

/* Return the number of bytes in the free blocks, unmapped ones included. */
static word
large_free_bytes(void)
{
  word result;

  LOCK();
  result = GC_large_free_bytes;
  UNLOCK();
  return result;
}

/*
 * Allocate garbage in objects of `GARBAGE_BYTES` bytes until the number
 * of bytes allocated since the latest collection, as the collector
 * counts it, reaches `bytes`.
 */
static void
alloc_garbage_until(word bytes)
{
  for (;;) {
    word allocd;

    LOCK();
    allocd = GC_adj_bytes_allocd();
    UNLOCK();
    if (allocd >= bytes)
      break;
    alloc_garbage(bytes - allocd < 64 * 1024 ? bytes - allocd : 64 * 1024,
                  GARBAGE_BYTES);
  }
}

/*
 * Make a request of more bytes than the free blocks hold, and tell
 * whether it caused a collection.  The request does not cause more than
 * one.  The object is pointer-free, so it is not cleared (and its pages
 * are not touched), and it is freed explicitly, since a stale copy of its
 * address on the stack would keep it live, and add its size to the live
 * data, at the next collection.
 */
static GC_bool
request_collects(void)
{
  word gc_no = GC_get_gc_no();
  word n;
  void *p = GC_MALLOC_ATOMIC(large_free_bytes() + HBLKSIZE);

  TEST_ASSERT(p != NULL);
  n = GC_get_gc_no() - gc_no;
  GC_FREE(p);
  TEST_ASSERT(n <= 1);
  return n != 0;
}

/*
 * Return the midpoint of half of the volume and 10% of the live data.
 * The heap grows without a collection until the larger one of the two
 * has been allocated since the latest collection.
 */
static word
midway_to_wait(struct sizing_s s)
{
  return s.volume / 4 + percent_of(s.live, 10) / 2;
}

/*
 * Check the decisions between a collection and a heap growth when
 * a request finds no free blocks, with the live data of the last step
 * and the heap above the target.  The heap grows without a collection
 * until half of the volume, but at least 10% of the live data, has been
 * allocated since the latest collection.  With the live data above the
 * ceiling (the volume is 10% of the live data), a request after midway
 * between half of the volume and 10% of the live data grows the heap.
 * Then, with the heap at least twice the live data and the ceiling at the
 * heap size (thus the target is halfway between the live data and the
 * heap, and the volume is half of the live data at least), a request
 * after midway between 10% of the live data and half of the volume grows
 * the heap, and a request after 3/4 of the volume causes one collection.
 * A heap growth reaches the target, and is 10% of the live data at least.
 */
static void
check_growth_decisions(void)
{
  struct sizing_s s;
  word heap, blocks;

  GC_gcollect();
  s = read_sizing();
  heap = s.target + percent_of(s.live, 20);
  if (s.heap < heap) {
    TEST_ASSERT(GC_expand_hp(heap - s.heap) != 0);
    GC_gcollect();
    s = read_sizing();
  }
  TEST_ASSERT(s.heap > s.target && s.volume < percent_of(s.live, 15));
  alloc_garbage_until(midway_to_wait(s));
  TEST_ASSERT(!request_collects());
  TEST_ASSERT(read_sizing().heap > s.heap);

  GC_gcollect();
  s = read_sizing();
  heap = 2 * s.live;
  if (s.heap < heap)
    TEST_ASSERT(GC_expand_hp(heap - s.heap) != 0);
  heap = read_sizing().heap;
  /* The ceiling is 75% of the memory limit. */
  GC_set_memory_limit(heap / 3 * 4);
  GC_gcollect();
  s = read_sizing();
  TEST_ASSERT(s.target < s.heap && s.volume >= percent_of(s.live, 45));
  alloc_garbage_until(midway_to_wait(s));
  TEST_ASSERT(!request_collects());
  GC_gcollect();
  s = read_sizing();
  TEST_ASSERT(s.target < s.heap && s.volume / 2 > percent_of(s.live, 10));
  alloc_garbage_until(s.volume / 4 * 3);
  TEST_ASSERT(request_collects());

  LOCK();
  blocks = target_expansion_blocks(heap_target(), 1, 0);
  TEST_ASSERT(blocks >= divHBLKSZ(percent_of(live_bytes(), 10)));
  blocks = target_expansion_blocks(GC_heapsize + 64 * MiB, 1, 0);
  TEST_ASSERT(blocks >= divHBLKSZ(64 * MiB));
  UNLOCK();
  printf("Heap growth decisions: live %lu KiB, volume %lu KiB, target %lu KiB,"
         " heap %lu KiB\n",
         (unsigned long)(s.live >> 10), (unsigned long)(s.volume >> 10),
         (unsigned long)(s.target >> 10),
         (unsigned long)(read_sizing().heap >> 10));
  GC_set_memory_limit(TEST_MEMORY_LIMIT);
}

static void
test_heap_sizing(void)
{
  static const unsigned steps[]
      = { 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52 };
  word budget = GC_get_alloc_budget_percent();
  word ceiling_percent = GC_get_heap_ceiling_percent();
  word initial_heap;
  struct sizing_s s;
  GC_bool strict = !GC_is_incremental_mode() && !GC_is_disabled();
  size_t i;

  if (!strict)
    printf("Heap sizing not checked: incremental mode or collections"
           " disabled\n");
  GC_set_alloc_budget_percent(TEST_BUDGET_PERCENT);
  GC_set_memory_limit(TEST_MEMORY_LIMIT);
  GC_set_heap_ceiling_percent(75);
  TEST_ASSERT(GC_get_heap_ceiling_percent() == 75);
  initial_heap = read_sizing().heap;

  /*
   * The live data grows from 2 MiB, where the budget gives the volume,
   * to 52 MiB, above the heap ceiling of 48 MiB, where the volume is 10%
   * of the live data.
   */
  for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    check_step((word)steps[i] * MiB, GARBAGE_BYTES, strict, initial_heap);

  /*
   * Above the ceiling, garbage in large objects of 256 KiB to 4 MiB
   * (which a collection might not free in large enough runs of blocks).
   */
  for (i = 0; i < 5; i++)
    check_step(52 * MiB, (size_t)(256 << 10) << i, strict, initial_heap);
  if (strict)
    check_growth_decisions();

  /* Without the ceiling, the budget gives the volume, and no target. */
  GC_set_heap_ceiling_percent(0);
  s = read_sizing();
  TEST_ASSERT(close_to(s.volume, expected_volume(s.live, 0)));
  TEST_ASSERT(0 == s.target);
  GC_set_heap_ceiling_percent(75);

  /* Without collections, there is no target. */
  GC_disable();
  TEST_ASSERT(0 == read_sizing().target);
  GC_enable();

  live_list = NULL;
  live_list_bytes = 0;
  GC_set_memory_limit(0);
  GC_set_heap_ceiling_percent(ceiling_percent);
  GC_set_alloc_budget_percent(budget);
}

int
main(void)
{
#ifdef LINUX
  test_cgroup_trees();
#endif
  test_memory_limit();
  test_heap_sizing();
  printf("SUCCEEDED\n");
  return 0;
}
