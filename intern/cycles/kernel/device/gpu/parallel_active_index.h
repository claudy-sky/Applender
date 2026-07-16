/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

CCL_NAMESPACE_BEGIN

/* Given an array of states, build an array of indices for which the states
 * are active.
 *
 * Shared memory requirement is `sizeof(int) * (number_of_warps + 1)`. */

#include "kernel/device/gpu/block_sizes.h"
#include "util/atomic.h"

/* TODO: abstract more device differences, define `ccl_gpu_local_syncthreads`,
 * `ccl_gpu_thread_warp`, `ccl_gpu_warp_index`, `ccl_gpu_num_warps` for all devices
 * and keep device specific code in `compat.h`. */

#  ifndef __KERNEL_METAL__
template<typename IsActiveOp>
__device__
#  endif
    void
    gpu_parallel_active_index_array_impl(const uint num_states,
                                         ccl_global int *indices,
                                         ccl_global int *num_indices,
#  ifdef __KERNEL_METAL__
                                         const uint is_active,
                                         const uint blocksize,
                                         const int thread_index,
                                         const uint state_index,
                                         const int ccl_gpu_warp_size,
                                         const int thread_warp,
                                         const int warp_index,
                                         const int num_warps,
                                         threadgroup int *warp_offset)
{
#  else
                                          IsActiveOp is_active_op)
{
  extern ccl_gpu_shared int warp_offset[];

#    ifndef __KERNEL_METAL__
  const uint blocksize = ccl_gpu_block_dim_x;
#    endif

  const uint thread_index = ccl_gpu_thread_idx_x;
  const uint thread_warp = thread_index % ccl_gpu_warp_size;

  const uint warp_index = thread_index / ccl_gpu_warp_size;
  const uint num_warps = blocksize / ccl_gpu_warp_size;

  const uint state_index = ccl_gpu_block_idx_x * blocksize + thread_index;

  /* Test if state corresponding to this thread is active. */
  const uint is_active = (state_index < num_states) ? is_active_op(state_index) : 0;
#  endif
  /* For each thread within a warp compute how many other active states precede it. */
#if   defined(__KERNEL_METAL__)
  /* Equivalent to popcount(ballot & lower_lane_mask): simd_ballot() reflects only the lanes
   * that are executing this instruction (uniform here, see below), and popcount(ballot & mask)
   * counts predicate-true lanes with a lower lane id. simd_prefix_exclusive_sum() over
   * int(is_active) sums the predicate over lanes with a lower lane id within the executing
   * simdgroup -- an identical result for any is_active pattern, PROVIDED every lane in the
   * simdgroup reaches this point without divergent early return. That holds here: `is_active`
   * is set via a ternary against `state_index < num_states` (see the ccl_gpu_kernel_signature
   * caller), never an early return, so control flow is
   * uniform up to this point -- the same property the ccl_gpu_ballot() path below already
   * relies on. */
  const uint thread_offset = uint(ccl_gpu_simd_prefix_exclusive_sum(int(is_active)));
#else
  const uint thread_offset = popcount(ccl_gpu_ballot(is_active) &
                                      ccl_gpu_thread_mask(thread_warp));
#endif

  /* Last thread in warp stores number of active states for each warp. */
  if (thread_warp == ccl_gpu_warp_size - 1) {
    warp_offset[warp_index] = thread_offset + is_active;
  }

  ccl_gpu_syncthreads();

  /* Last thread in block converts per-warp sizes to offsets, increments global size of
   * index array and gets offset to write to. */
#ifdef __KERNEL_METAL__
  /* Parallel cross-warp exclusive scan, performed by the first simdgroup only (thread_warp is
   * this thread's lane index within its own simdgroup; warp_index==0 selects the first
   * simdgroup of the threadgroup). This produces exactly the same warp_offset[] layout as the
   * serial scan in the fallback below: warp_offset[i] becomes the exclusive prefix sum of
   * per-warp active counts, and warp_offset[num_warps] becomes the block's global base offset
   * (both are what the "write to index array" code below reads back).
   * Requires num_warps <= ccl_gpu_warp_size. This holds for every kernel that instantiates this
   * template on Apple GPUs: threadgroup size is capped at 1024 with a 32-wide simdgroup, so
   * num_warps = blocksize / 32 <= 32 == ccl_gpu_warp_size always -- see queue.mm, which sizes
   * threadgroup memory from this same invariant. Guarded at runtime regardless, falling back to
   * the serial scan otherwise. */
  if (num_warps <= ccl_gpu_warp_size) {
    if (warp_index == 0) {
      const int v = (thread_warp < num_warps) ? warp_offset[thread_warp] : 0;
      const int excl = ccl_gpu_simd_prefix_exclusive_sum(v);
      if (thread_warp < num_warps) {
        warp_offset[thread_warp] = excl;
      }
      if (thread_warp == num_warps - 1) {
        warp_offset[num_warps] = atomic_fetch_and_add_uint32(num_indices, excl + v);
      }
    }
  }
  else if (thread_index == blocksize - 1) {
    /* Fallback serial scan (identical to the non-Metal path below) for the case that a future
     * block size violates num_warps <= ccl_gpu_warp_size. */
    int offset = 0;
    for (int i = 0; i < num_warps; i++) {
      int num_active = warp_offset[i];
      warp_offset[i] = offset;
      offset += num_active;
    }

    const uint block_num_active = warp_offset[warp_index] + thread_offset + is_active;
    warp_offset[num_warps] = atomic_fetch_and_add_uint32(num_indices, block_num_active);
  }
#else
  if (thread_index == blocksize - 1) {
    /* TODO: parallelize this. */
    int offset = 0;
    for (int i = 0; i < num_warps; i++) {
      int num_active = warp_offset[i];
      warp_offset[i] = offset;
      offset += num_active;
    }

    const uint block_num_active = warp_offset[warp_index] + thread_offset + is_active;
    warp_offset[num_warps] = atomic_fetch_and_add_uint32(num_indices, block_num_active);
  }
#endif

  ccl_gpu_syncthreads();

  /* Write to index array. */
  if (is_active) {
    const uint block_offset = warp_offset[num_warps];
    indices[block_offset + warp_offset[warp_index] + thread_offset] = state_index;
  }
}

#ifdef __KERNEL_METAL__

#  define gpu_parallel_active_index_array(num_states, indices, num_indices, is_active_op) \
    const uint is_active = (ccl_gpu_global_id_x() < num_states) ? \
                               is_active_op(ccl_gpu_global_id_x()) : \
                               0; \
    gpu_parallel_active_index_array_impl(num_states, \
                                         indices, \
                                         num_indices, \
                                         is_active, \
                                         metal_local_size, \
                                         metal_local_id, \
                                         metal_global_id, \
                                         simdgroup_size, \
                                         simd_lane_index, \
                                         simd_group_index, \
                                         num_simd_groups, \
                                         (threadgroup int *)threadgroup_array)
#else

#  define gpu_parallel_active_index_array(num_states, indices, num_indices, is_active_op) \
    gpu_parallel_active_index_array_impl(num_states, indices, num_indices, is_active_op)

#endif

CCL_NAMESPACE_END
