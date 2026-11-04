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
 * Test that the heap can be expanded by one block twice as many times as
 * there are entries for the memory which the collector gets from the
 * system (`MAX_HEAP_SECTS`), where every chunk of that memory is recorded
 * in such an entry (`USE_PROC_FOR_LIBRARIES`, defined if `malloc` is
 * redirected on Linux with threads).  The chunks of consecutive expansions
 * adjoin each other, thus they need few entries.  The test is skipped in
 * the other builds, with `LARGE_CONFIG`, where the expansions would take
 * long (the heap sections are checked against each other on every
 * expansion with the assertions on), and without `USE_MMAP`, where the
 * heap grows upward by `sbrk`: each heap section above the others then
 * takes a static-root exclusion (`INCLUDE_LINUX_THREAD_DESCR`), and there
 * are fewer of those (`MAX_EXCLUSIONS`) than expansions.  For the same
 * reason the test aborts with "Too many exclusions" where `mmap` places
 * new mappings upward, as Linux does on arm64 with an unlimited stack size,
 * and on x86 only with the legacy layout (`setarch -L`, or the
 * `vm.legacy_va_layout` sysctl).
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#define NOT_GCBUILD
#include "private/gc_priv.h"

#include <stdio.h>

int
main(void)
{
#if defined(USE_PROC_FOR_LIBRARIES) && defined(USE_MMAP) \
    && !defined(LARGE_CONFIG)
  int i;

  GC_INIT();
  for (i = 0; i < 2 * MAX_HEAP_SECTS; i++) {
    if (!GC_expand_hp(HBLKSIZE)) {
      fprintf(stderr, "Heap expansion %d failed\n", i);
      return 1;
    }
  }
  printf("SUCCEEDED (%d heap expansions, heap size: %lu KiB)\n", i,
         (unsigned long)(GC_get_heap_size() >> 10));
#else
  printf("test skipped\n");
#endif
  return 0;
}
