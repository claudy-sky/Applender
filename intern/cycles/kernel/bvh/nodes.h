/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "kernel/geom/object.h"
#include "kernel/globals.h"

CCL_NAMESPACE_BEGIN

#if defined(__KERNEL_NEON_NATIVE__)
/* Lane-wise (a < b) ? a : b and (a > b) ? a : b. Unlike vmin/vmax these
 * return the second operand when a lane compares unordered (NaN), matching
 * the scalar min/max the slab test below relies on: a NaN slab distance
 * (0 * inf) must lose against the other operand instead of propagating. */
ccl_device_forceinline float32x2_t bvh_neon_min(const float32x2_t a, const float32x2_t b)
{
  return vbsl_f32(vclt_f32(a, b), a, b);
}

ccl_device_forceinline float32x2_t bvh_neon_max(const float32x2_t a, const float32x2_t b)
{
  return vbsl_f32(vcgt_f32(a, b), a, b);
}
#endif

// TODO(sergey): Look into avoid use of full Transform and use 3x3 matrix and
// 3-vector which might be faster.
ccl_device_forceinline Transform bvh_unaligned_node_fetch_space(KernelGlobals kg,
                                                                const int node_addr,
                                                                const int child)
{
  Transform space;
  const int child_addr = node_addr + child * 3;
  space.x = kernel_data_fetch(bvh_nodes, child_addr + 1);
  space.y = kernel_data_fetch(bvh_nodes, child_addr + 2);
  space.z = kernel_data_fetch(bvh_nodes, child_addr + 3);
  return space;
}

ccl_device_forceinline int bvh_aligned_node_intersect(KernelGlobals kg,
                                                      const float3 P,
                                                      const float3 idir,
                                                      const float tmin,
                                                      const float tmax,
                                                      const int node_addr,
                                                      const uint visibility,
                                                      float dist[2])
{

  /* fetch node data */
#ifdef __VISIBILITY_FLAG__
  float4 cnodes = kernel_data_fetch(bvh_nodes, node_addr + 0);
#endif
  float4 node0 = kernel_data_fetch(bvh_nodes, node_addr + 1);
  float4 node1 = kernel_data_fetch(bvh_nodes, node_addr + 2);
  float4 node2 = kernel_data_fetch(bvh_nodes, node_addr + 3);

  /* intersect ray against child nodes */
#if defined(__KERNEL_NEON_NATIVE__)
  /* Slab test for both children at once. Lanes of nodeK are
   * (c0lo, c1lo, c0hi, c1hi) for one axis, so the low/high vector halves
   * hold the lower/upper slab distances of child 0 and child 1. */
  const float32x4_t tx = vmulq_laneq_f32(
      vsubq_f32(node0.m128, vdupq_laneq_f32(P.m128, 0)), idir.m128, 0);
  const float32x4_t ty = vmulq_laneq_f32(
      vsubq_f32(node1.m128, vdupq_laneq_f32(P.m128, 1)), idir.m128, 1);
  const float32x4_t tz = vmulq_laneq_f32(
      vsubq_f32(node2.m128, vdupq_laneq_f32(P.m128, 2)), idir.m128, 2);

  const float32x2_t near_x = bvh_neon_min(vget_low_f32(tx), vget_high_f32(tx));
  const float32x2_t far_x = bvh_neon_max(vget_low_f32(tx), vget_high_f32(tx));
  const float32x2_t near_y = bvh_neon_min(vget_low_f32(ty), vget_high_f32(ty));
  const float32x2_t far_y = bvh_neon_max(vget_low_f32(ty), vget_high_f32(ty));
  const float32x2_t near_z = bvh_neon_min(vget_low_f32(tz), vget_high_f32(tz));
  const float32x2_t far_z = bvh_neon_max(vget_low_f32(tz), vget_high_f32(tz));

  /* Same association as the scalar max4/min4 below. */
  const float32x2_t cmin = bvh_neon_max(bvh_neon_max(vdup_n_f32(tmin), near_x),
                                        bvh_neon_max(near_y, near_z));
  const float32x2_t cmax = bvh_neon_min(bvh_neon_min(vdup_n_f32(tmax), far_x),
                                        bvh_neon_min(far_y, far_z));

  vst1_f32(dist, cmin);

  const uint32x2_t valid = vcge_f32(cmax, cmin);

#  ifdef __VISIBILITY_FLAG__
  return ((vget_lane_u32(valid, 0) && (__float_as_uint(cnodes.x) & visibility)) ? 1 : 0) |
         ((vget_lane_u32(valid, 1) && (__float_as_uint(cnodes.y) & visibility)) ? 2 : 0);
#  else
  return (vget_lane_u32(valid, 0) ? 1 : 0) | (vget_lane_u32(valid, 1) ? 2 : 0);
#  endif
#else
  float c0lox = (node0.x - P.x) * idir.x;
  float c0hix = (node0.z - P.x) * idir.x;
  float c0loy = (node1.x - P.y) * idir.y;
  float c0hiy = (node1.z - P.y) * idir.y;
  float c0loz = (node2.x - P.z) * idir.z;
  float c0hiz = (node2.z - P.z) * idir.z;
  float c0min = max4(tmin, min(c0lox, c0hix), min(c0loy, c0hiy), min(c0loz, c0hiz));
  float c0max = min4(tmax, max(c0lox, c0hix), max(c0loy, c0hiy), max(c0loz, c0hiz));

  float c1lox = (node0.y - P.x) * idir.x;
  float c1hix = (node0.w - P.x) * idir.x;
  float c1loy = (node1.y - P.y) * idir.y;
  float c1hiy = (node1.w - P.y) * idir.y;
  float c1loz = (node2.y - P.z) * idir.z;
  float c1hiz = (node2.w - P.z) * idir.z;
  float c1min = max4(tmin, min(c1lox, c1hix), min(c1loy, c1hiy), min(c1loz, c1hiz));
  float c1max = min4(tmax, max(c1lox, c1hix), max(c1loy, c1hiy), max(c1loz, c1hiz));

  dist[0] = c0min;
  dist[1] = c1min;

#  ifdef __VISIBILITY_FLAG__
  /* this visibility test gives a 5% performance hit, how to solve? */
  return (((c0max >= c0min) && (__float_as_uint(cnodes.x) & visibility)) ? 1 : 0) |
         (((c1max >= c1min) && (__float_as_uint(cnodes.y) & visibility)) ? 2 : 0);
#  else
  return ((c0max >= c0min) ? 1 : 0) | ((c1max >= c1min) ? 2 : 0);
#  endif
#endif
}

ccl_device_forceinline bool bvh_unaligned_node_intersect_child(KernelGlobals kg,
                                                               const float3 P,
                                                               const float3 dir,
                                                               const float tmin,
                                                               const float tmax,
                                                               const int node_addr,
                                                               const int child,
                                                               float dist[2])
{
  Transform space = bvh_unaligned_node_fetch_space(kg, node_addr, child);
  float3 aligned_dir = transform_direction(&space, dir);
  float3 aligned_P = transform_point(&space, P);
  float3 nrdir = -bvh_inverse_direction(aligned_dir);
  float3 lower_xyz = aligned_P * nrdir;
  float3 upper_xyz = lower_xyz - nrdir;
  const float near_x = min(lower_xyz.x, upper_xyz.x);
  const float near_y = min(lower_xyz.y, upper_xyz.y);
  const float near_z = min(lower_xyz.z, upper_xyz.z);
  const float far_x = max(lower_xyz.x, upper_xyz.x);
  const float far_y = max(lower_xyz.y, upper_xyz.y);
  const float far_z = max(lower_xyz.z, upper_xyz.z);
  const float tnear = max4(tmin, near_x, near_y, near_z);
  const float tfar = min4(tmax, far_x, far_y, far_z);
  *dist = tnear;
  return tnear <= tfar;
}

ccl_device_forceinline int bvh_unaligned_node_intersect(KernelGlobals kg,
                                                        const float3 P,
                                                        const float3 dir,
                                                        const float tmin,
                                                        const float tmax,
                                                        const int node_addr,
                                                        const uint visibility,
                                                        float dist[2])
{
  int mask = 0;
#ifdef __VISIBILITY_FLAG__
  float4 cnodes = kernel_data_fetch(bvh_nodes, node_addr + 0);
#endif
  if (bvh_unaligned_node_intersect_child(kg, P, dir, tmin, tmax, node_addr, 0, &dist[0])) {
#ifdef __VISIBILITY_FLAG__
    if ((__float_as_uint(cnodes.x) & visibility))
#endif
    {
      mask |= 1;
    }
  }
  if (bvh_unaligned_node_intersect_child(kg, P, dir, tmin, tmax, node_addr, 1, &dist[1])) {
#ifdef __VISIBILITY_FLAG__
    if ((__float_as_uint(cnodes.y) & visibility))
#endif
    {
      mask |= 2;
    }
  }
  return mask;
}

ccl_device_forceinline int bvh_node_intersect(KernelGlobals kg,
                                              const float3 P,
                                              const float3 dir,
                                              const float3 idir,
                                              const float tmin,
                                              const float tmax,
                                              const int node_addr,
                                              const uint visibility,
                                              float dist[2])
{
  float4 node = kernel_data_fetch(bvh_nodes, node_addr);
  if (__float_as_uint(node.x) & PATH_RAY_VISIBILITY_NODE_UNALIGNED) {
    return bvh_unaligned_node_intersect(kg, P, dir, tmin, tmax, node_addr, visibility, dist);
  }
  return bvh_aligned_node_intersect(kg, P, idir, tmin, tmax, node_addr, visibility, dist);
}

CCL_NAMESPACE_END
