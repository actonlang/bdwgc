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
 * Check the count of the processors on Linux: the parsing of synthetic
 * `/proc/stat` pseudo-files, one of which lists 256 processors in long
 * lines, and that the count of the processors of the system is one plus
 * the maximum `N` of the lines "cpu<N>" of `/proc/stat` pseudo-file read
 * in full by stdio.  Check the count of the processors which are not
 * efficiency cores on Linux, in synthetic sysfs trees of Intel hybrid
 * processors and of ARM processors of one to three classes, and the
 * count of the processors which the default number of markers is based
 * on, which is at least 40% of all of them: for these trees, for the
 * counts of some Apple and Intel processors, and, on Linux and macOS,
 * that the collector initialization sets the default number of markers
 * from that count of the system.  This test includes the collector
 * source to call its static functions.
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

#define TEST_ASSERT(e)                                                      \
  do {                                                                      \
    if (!(e)) {                                                             \
      fprintf(stderr, "Assertion failure: %s:%d, %s\n", __FILE__, __LINE__, \
              #e);                                                          \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

/* `GC_parse_proc_stat()` exists where this macro is defined. */
#ifdef PROC_STAT_BUF_SZ

/*
 * Return the number of processors which `GC_parse_proc_stat()` counts
 * in the contents written to the temporary file `f`, and close `f`.
 */
static int
parse_tmpfile(FILE *f)
{
  int result;

  TEST_ASSERT(fflush(f) == 0 && !ferror(f));
  TEST_ASSERT(lseek(fileno(f), 0, SEEK_SET) == 0);
  result = GC_parse_proc_stat(fileno(f));
  TEST_ASSERT(fclose(f) == 0);
  return result;
}

/*
 * Return the number of processors which `GC_parse_proc_stat()` counts
 * in a file of the given contents.
 */
static int
parse_proc_stat(const char *contents)
{
  FILE *f = tmpfile();
  size_t len = strlen(contents);

  TEST_ASSERT(f != NULL);
  TEST_ASSERT(fwrite(contents, 1, len, f) == len);
  return parse_tmpfile(f);
}

/* The number of processors of the big synthetic file. */
#  define N_BIG_CPUS 256

/* The number of interrupt counts of the big synthetic file. */
#  define N_BIG_INTERRUPTS 4096

static void
test_proc_stat(void)
{
  FILE *f = tmpfile();
  int i, j;

  /*
   * The big file has the line of the totals and a line per processor,
   * each with 10 numbers of 20 digits (over 200 bytes), then the lines
   * of the interrupt counts and of the context switches.
   */
  TEST_ASSERT(f != NULL);
  for (i = -1; i < N_BIG_CPUS; i++) {
    if (i < 0) {
      fputs("cpu ", f);
    } else {
      fprintf(f, "cpu%d", i);
    }
    for (j = 0; j < 10; j++) {
      fprintf(f, " 1234567890123456789%d", j);
    }
    fputc('\n', f);
  }
  fputs("intr", f);
  for (j = 0; j < N_BIG_INTERRUPTS; j++) {
    fputs(" 0", f);
  }
  fputs("\nctxt 123456789\n", f);
  TEST_ASSERT(parse_tmpfile(f) == N_BIG_CPUS);

  /* The count is one plus the maximum processor number. */
  TEST_ASSERT(parse_proc_stat("cpu  1 2 3 4\n"
                              "cpu0 1 2 3 4\n"
                              "cpu1 1 2 3 4\n"
                              "cpu3 1 2 3 4\n"
                              "cpu12 1 2 3 4\n"
                              "intr 5 0 0\n"
                              "ctxt 6\n")
              == 13);
  TEST_ASSERT(parse_proc_stat("cpu  1 2 3 4\n"
                              "intr 5 0 0\n")
              == 1);
  TEST_ASSERT(parse_proc_stat("") == 1);
}

/*
 * Return one plus the maximum `N` of the lines "cpu<N>" of `/proc/stat`
 * pseudo-file, read in full by stdio.
 */
static int
count_proc_stat_cpus(void)
{
  static char line[4096];
  FILE *f = fopen("/proc/stat", "r");
  int result = 1;
  int at_line_start = 1;

  TEST_ASSERT(f != NULL);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (at_line_start && strncmp(line, "cpu", 3) == 0 && line[3] >= '0'
        && line[3] <= '9') {
      int cpu_no = atoi(&line[3]);

      if (cpu_no >= result)
        result = cpu_no + 1;
    }
    /* A line longer than the buffer continues in the next one. */
    at_line_start = strchr(line, '\n') != NULL;
  }
  TEST_ASSERT(!ferror(f));
  fclose(f);
  return result;
}

#endif /* PROC_STAT_BUF_SZ */

/* `GC_sysfs_performance_nprocs()` exists where this macro is defined. */
#ifdef SYSFS_BUF_SZ

/*
 * The root of the synthetic sysfs trees.  Its size leaves room for the
 * paths in the trees.
 */
static char tree_root[SYSFS_PATH_SZ - 64];

/* The files and directories created in the root, to remove them. */
#  define MAX_TREE_PATHS (2 * MAX_MARKERS + 16)
static char tree_paths[MAX_TREE_PATHS][SYSFS_PATH_SZ];
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
 * Add the capacities of the processors from `first` to `last` to the
 * synthetic tree.
 */
static void
add_tree_capacities(int first, int last, int capacity)
{
  char rel[64];
  char contents[16];
  int i;

  TEST_ASSERT(snprintf(contents, sizeof(contents), "%d\n", capacity) > 0);
  for (i = first; i <= last; i++) {
    int len = snprintf(rel, sizeof(rel),
                       "/devices/system/cpu/cpu%d/cpu_capacity", i);

    TEST_ASSERT(len > 0 && (size_t)len < sizeof(rel));
    add_tree_file(rel, contents);
  }
}

/*
 * Check the count of the processors which are not efficiency cores (0 if
 * no processor is an efficiency core) in the synthetic tree of `nprocs`
 * processors, and the count of the processors which the default number
 * of markers is based on, and remove the tree.
 */
static void
check_tree(int nprocs, int expected, int expected_for_markers)
{
  int count = GC_sysfs_performance_nprocs(tree_root, nprocs);
  int for_markers = GC_nprocs_for_markers(count, nprocs);

  if (count != expected || for_markers != expected_for_markers) {
    fprintf(stderr,
            "Synthetic tree of %d processors: %d not efficiency cores"
            " (%d expected), %d for markers (%d expected)\n",
            nprocs, count, expected, for_markers, expected_for_markers);
    exit(1);
  }
  while (n_tree_paths > 0) {
    const char *path = tree_paths[--n_tree_paths];

    TEST_ASSERT(remove(path) == 0);
  }
}

static void
test_sysfs(void)
{
  /* `/tmp` does not exist everywhere, e.g. on Android. */
  const char *dir = getenv("TMPDIR");
  int len;

  if (NULL == dir || '\0' == *dir)
    dir = "/tmp";
  len = snprintf(tree_root, sizeof(tree_root), "%s/cpucounttest.XXXXXX", dir);
  TEST_ASSERT(len > 0 && (size_t)len < sizeof(tree_root));
  TEST_ASSERT(mkdtemp(tree_root) != NULL);

  /*
   * Intel hybrid processors: 8 two-thread performance cores and 8
   * efficiency cores, 8 two-thread performance cores and 16 efficiency
   * cores, and 2 two-thread performance cores and 10 efficiency cores
   * (e.g. Meteor Lake U), where the 4 processors which are not efficiency
   * cores are fewer than 40% of the 14.
   */
  add_tree_file("/devices/cpu_core/cpus", "0-15\n");
  add_tree_file("/devices/cpu_atom/cpus", "16-23\n");
  check_tree(24, 16, 16);
  add_tree_file("/devices/cpu_core/cpus", "0-15\n");
  add_tree_file("/devices/cpu_atom/cpus", "16-31\n");
  check_tree(32, 16, 16);
  add_tree_file("/devices/cpu_core/cpus", "0-3\n");
  add_tree_file("/devices/cpu_atom/cpus", "4-13\n");
  check_tree(14, 4, 6);

  /* Lists of several ranges and of single processors. */
  add_tree_file("/devices/cpu_core/cpus", "0-3,8,10-12\n");
  add_tree_file("/devices/cpu_atom/cpus", "4-7,9,13-23\n");
  check_tree(24, 8, 10);

  /* Fewer processors than performance cores, e.g. in a container. */
  add_tree_file("/devices/cpu_core/cpus", "0-15\n");
  add_tree_file("/devices/cpu_atom/cpus", "16-23\n");
  check_tree(4, 0, 4);

  /* No efficiency core is online. */
  add_tree_file("/devices/cpu_core/cpus", "0-15\n");
  add_tree_file("/devices/cpu_atom/cpus", "\n");
  check_tree(16, 0, 16);

  /* Two classes of ARM processors. */
  add_tree_capacities(0, 3, 446);
  add_tree_capacities(4, 7, 1024);
  check_tree(8, 4, 4);

  /* Three classes, the most capable one first. */
  add_tree_capacities(0, 0, 1024);
  add_tree_capacities(1, 3, 870);
  add_tree_capacities(4, 7, 325);
  check_tree(8, 4, 4);

  /* Capacities which differ less, e.g. by the maximum frequencies. */
  add_tree_capacities(0, 11, 1024);
  add_tree_capacities(12, 15, 870);
  check_tree(16, 0, 16);

  /* The lowest capacity just below 3/4 of the highest one, and at it. */
  add_tree_capacities(0, 3, 1024);
  add_tree_capacities(4, 7, 767);
  check_tree(8, 4, 4);
  add_tree_capacities(0, 3, 1024);
  add_tree_capacities(4, 7, 768);
  check_tree(8, 0, 8);

  /*
   * Efficiency cores after `MAX_MARKERS - 1` processors of the highest
   * capacity, and after `MAX_MARKERS` ones, which end the reading.
   */
  add_tree_capacities(0, MAX_MARKERS - 2, 1024);
  add_tree_capacities(MAX_MARKERS - 1, MAX_MARKERS + 2, 446);
  check_tree(MAX_MARKERS + 3, MAX_MARKERS - 1, MAX_MARKERS - 1);
  add_tree_capacities(0, MAX_MARKERS - 1, 1024);
  add_tree_capacities(MAX_MARKERS, MAX_MARKERS + 3, 446);
  check_tree(MAX_MARKERS + 4, 0, MAX_MARKERS + 4);

  /* One class. */
  add_tree_capacities(0, 7, 1024);
  check_tree(8, 0, 8);

  /* The capacity of a processor is missing. */
  add_tree_capacities(0, 1, 446);
  add_tree_capacities(3, 3, 1024);
  check_tree(4, 0, 4);

  /* No capacities, as on most x86 processors. */
  check_tree(8, 0, 8);

  TEST_ASSERT(rmdir(tree_root) == 0);
}

#endif /* SYSFS_BUF_SZ */

/*
 * `GC_get_performance_nprocs()` and `GC_nprocs_for_markers()` exist where
 * this macro is defined.
 */
#if defined(PARALLEL_MARK) && (defined(DARWIN) || defined(LINUX))
#  define HAVE_PERFORMANCE_NPROCS

/*
 * Check the count of the processors which the default number of markers
 * is based on, given that `performance_nprocs` of the `nprocs` processors
 * are not efficiency cores (0 if no processor is an efficiency core).
 */
static void
check_nprocs_for_markers(int performance_nprocs, int nprocs, int expected)
{
  int count = GC_nprocs_for_markers(performance_nprocs, nprocs);

  if (count != expected) {
    fprintf(stderr,
            "%d of %d processors not efficiency cores: %d for markers,"
            " %d expected\n",
            performance_nprocs, nprocs, count, expected);
    exit(1);
  }
}

static void
test_nprocs_for_markers(void)
{
  /* No efficiency cores: all the processors. */
  check_nprocs_for_markers(0, 8, 8);
  check_nprocs_for_markers(0, 1, 1);

  /* Apple M4 Pro (10 + 4), M6 (2 + 4 + 6) and M1 (4 + 4). */
  check_nprocs_for_markers(10, 14, 10);
  check_nprocs_for_markers(6, 12, 6);
  check_nprocs_for_markers(4, 8, 4);

  /* Intel: 8 two-thread performance cores and 16 efficiency cores. */
  check_nprocs_for_markers(16, 32, 16);

  /*
   * Intel: 2 two-thread performance cores and 10 efficiency cores (e.g.
   * Meteor Lake U), and 2 two-thread performance cores and 8 efficiency
   * cores (e.g. Alder Lake U), where 40% of all the processors, rounded
   * up, are counted.
   */
  check_nprocs_for_markers(4, 14, 6);
  check_nprocs_for_markers(4, 12, 5);

  /* Exactly 40%, just below it, and one of two processors. */
  check_nprocs_for_markers(2, 5, 2);
  check_nprocs_for_markers(2, 6, 3);
  check_nprocs_for_markers(1, 2, 1);
}

/*
 * Check that the collector initialization sets the default number of
 * markers from the count of the processors which are not efficiency
 * cores, raised to 40% of all the processors if it is below that.
 */
static void
test_default_markers(void)
{
  int performance_nprocs, for_markers, expected;

  GC_INIT();
  performance_nprocs = GC_get_performance_nprocs(GC_nprocs);
  for_markers = GC_nprocs_for_markers(performance_nprocs, GC_nprocs);
  printf("Processors: %d, not efficiency cores: %d (0: no exclusion),"
         " counted for markers: %d, markers: %d\n",
         GC_nprocs, performance_nprocs, for_markers,
         GC_available_markers_m1 + 1);
  TEST_ASSERT(performance_nprocs >= 0 && performance_nprocs < GC_nprocs);
  TEST_ASSERT(for_markers > 0 && for_markers <= GC_nprocs);
  if (getenv("GC_MARKERS") != NULL || getenv("GC_NPROCS") != NULL) {
    printf("Markers not checked: GC_MARKERS or GC_NPROCS is set\n");
    return;
  }
  expected = for_markers;
#  ifdef GC_MIN_MARKERS
  if (expected < GC_MIN_MARKERS)
    expected = GC_MIN_MARKERS;
#  endif
#  if defined(GC_MARKERS) && defined(GC_THREADS)
  /*
   * `GC_INIT()` passes it to `GC_set_markers_count()`, where 0 keeps the
   * default.
   */
  if (GC_MARKERS > 0)
    expected = GC_MARKERS;
#  endif
  if (expected > MAX_MARKERS)
    expected = MAX_MARKERS;
  TEST_ASSERT(GC_available_markers_m1 + 1 == expected);
}

#endif

int
main(void)
{
#ifdef PROC_STAT_BUF_SZ
  int nprocs = GC_get_nprocs();
  int listed = count_proc_stat_cpus();

  test_proc_stat();
  printf("Processors: %d, listed in /proc/stat: %d, online (sysconf): %ld\n",
         nprocs, listed, (long)sysconf(_SC_NPROCESSORS_ONLN));
  TEST_ASSERT(nprocs == listed);
#endif
#ifdef SYSFS_BUF_SZ
  test_sysfs();
#endif
#ifdef HAVE_PERFORMANCE_NPROCS
  test_nprocs_for_markers();
  test_default_markers();
#endif
#if defined(PROC_STAT_BUF_SZ) || defined(HAVE_PERFORMANCE_NPROCS)
  printf("SUCCEEDED\n");
#else
  printf("Skipped (no processor counts to check)\n");
#endif
  return 0;
}
