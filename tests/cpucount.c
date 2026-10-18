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
 * in full by stdio.  This test includes the collector source to call its
 * static functions.
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
  printf("SUCCEEDED\n");
#else
  printf("Skipped (no /proc/stat parsing)\n");
#endif
  return 0;
}
