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
 * This test includes the collector source to call its static functions.
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

int
main(void)
{
#ifdef LINUX
  test_cgroup_trees();
#endif
  test_memory_limit();
  printf("SUCCEEDED\n");
  return 0;
}
