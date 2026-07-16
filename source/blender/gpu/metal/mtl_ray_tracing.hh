/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Acceleration structure support for hardware ray tracing (ray queries).
 *
 * Bottom level acceleration structures are built from triangle geometry
 * (vertex + index buffer pairs). Top level acceleration structures reference
 * bottom level ones through instance descriptors.
 *
 * Only available when `GPU_ray_query_support()` is true, which requires the
 * `BLENDER_METAL_RAYTRACING=1` environment variable opt-in and a device with
 * hardware ray tracing support (see `MTLBackend::capabilities_init`).
 */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_ray_tracing.hh"

#include "mtl_memory.hh"

#include <Metal/Metal.h>

namespace blender::gpu {

class MTLContext;

class MTLBottomLevelAS : public BottomLevelAS {
 private:
  /** Built acceleration structure. Nil until `build()` has been encoded. */
  id<MTLAccelerationStructure> accel_struct_ = nil;
  /** Geometry descriptors accumulated by `add_geometry()`. */
  NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor *> *geometry_descriptors_ =
      nil;

 public:
  MTLBottomLevelAS(const char *name);
  ~MTLBottomLevelAS() override;

  void add_geometry(IndexBuf &index_buffer, VertBuf &vertex_buffer) override;
  void build() override;

  id<MTLAccelerationStructure> acceleration_structure() const
  {
    return accel_struct_;
  }
};

class MTLTopLevelAS : public TopLevelAS {
 private:
  /** Built acceleration structure. Nil until `build()` has been encoded. */
  id<MTLAccelerationStructure> accel_struct_ = nil;
  /** CPU side instance descriptors, uploaded to `instance_buffer_` at build time. */
  Vector<MTLAccelerationStructureInstanceDescriptor> instances_;
  /** Bottom level acceleration structure of each instance. Also needed at bind time to make the
   * indirectly referenced acceleration structures resident. */
  Vector<const MTLBottomLevelAS *> blas_per_instance_;
  /** GPU buffer holding `instances_`. */
  gpu::MTLBuffer *instance_buffer_ = nullptr;
  bool is_dirty_ = true;

 public:
  MTLTopLevelAS(const char *name);
  ~MTLTopLevelAS() override;

  InstanceID add_instance(const BottomLevelAS &blas, const float4x4 &mat, uint8_t mask) override;
  void update_instance(InstanceID instance_id, const float4x4 &mat, uint8_t mask) override;
  void build() override;
  void bind(int slot) override;

  id<MTLAccelerationStructure> acceleration_structure() const
  {
    return accel_struct_;
  }

  Span<const MTLBottomLevelAS *> instanced_acceleration_structures() const
  {
    return blas_per_instance_;
  }
};

static inline MTLTopLevelAS *unwrap(TopLevelAS *tlas)
{
  return static_cast<MTLTopLevelAS *>(tlas);
}
static inline const MTLBottomLevelAS &unwrap(const BottomLevelAS &blas)
{
  return static_cast<const MTLBottomLevelAS &>(blas);
}

}  // namespace blender::gpu
