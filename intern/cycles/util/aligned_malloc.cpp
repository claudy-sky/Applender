/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/aligned_malloc.h"
#include "util/guarded_allocator.h"

#ifdef WITH_BLENDER_GUARDEDALLOC
#  include "../../guardedalloc/MEM_guardedalloc.h"
#endif

#include <cassert>

/* Adopted from Libmv. */

/* Apple's `malloc` is 16-byte aligned, and does not have `malloc.h`, so include
 * `stdlib` instead.
 */
#include <cstdlib>

CCL_NAMESPACE_BEGIN

void *util_aligned_malloc(const size_t size, const int alignment)
{
  void *mem = nullptr;
#ifdef WITH_BLENDER_GUARDEDALLOC
  mem = MEM_new_uninitialized_aligned(size, alignment, "Cycles Aligned Alloc");
#else
  if (posix_memalign(&mem, alignment, size)) {
    /* Non-zero means allocation error
     * either no allocation or bad alignment value. */
    mem = nullptr;
  }
#endif
  if (mem) {
    util_guarded_mem_alloc(size);
  }
  return mem;
}

void util_aligned_free(void *ptr, const size_t size)
{
  if (ptr) {
    util_guarded_mem_free(size);
  }
#if defined(WITH_BLENDER_GUARDEDALLOC)
  if (ptr != nullptr) {
    MEM_delete_void(ptr);
  }
#else
  free(ptr);
#endif
}

CCL_NAMESPACE_END
