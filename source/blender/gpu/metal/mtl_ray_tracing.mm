/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "mtl_ray_tracing.hh"

#include "mtl_backend.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_index_buffer.hh"
#include "mtl_vertex_buffer.hh"

#include "GPU_capabilities.hh"

namespace blender::gpu {

/* Metal does not expose a queryable instance count limit.
 * The instance descriptor `accelerationStructureIndex` is a `uint32_t`, but the effective
 * limit documented for hardware ray tracing is 2^24 instances. */
#define MTL_MAX_TLAS_INSTANCES (1 << 24)

/* -------------------------------------------------------------------- */
/** \name Common build encoding
 * \{ */

/* Encode a build into the context's command stream and return the retained acceleration
 * structure. Returns nil on failure. */
static id<MTLAccelerationStructure> mtl_acceleration_structure_build(
    MTLContext &ctx, MTLAccelerationStructureDescriptor *descriptor, const char *name)
{
  id<MTLDevice> device = ctx.device;

  const MTLAccelerationStructureSizes sizes = [device
      accelerationStructureSizesWithDescriptor:descriptor];

  id<MTLAccelerationStructure> accel_struct = [device
      newAccelerationStructureWithSize:sizes.accelerationStructureSize];
  if (accel_struct == nil) {
    MTL_LOG_ERROR("Could not allocate acceleration structure storage for '%s' (%llu bytes).",
                  name,
                  (unsigned long long)sizes.accelerationStructureSize);
    return nil;
  }
#ifndef NDEBUG
  accel_struct.label = [NSString stringWithUTF8String:name];
#endif

  /* Scratch memory only needs to live for the duration of the build on the GPU timeline.
   * The pool free is deferred until in-flight command buffers complete.
   * A non-empty scratch buffer is required even if the reported scratch size is zero. */
  const uint64_t scratch_size = (sizes.buildScratchBufferSize > 256) ?
                                    sizes.buildScratchBufferSize :
                                    256;
  gpu::MTLBuffer *scratch_buffer = MTLContext::get_global_memory_manager()->allocate(scratch_size,
                                                                                     false);
  BLI_assert(scratch_buffer != nullptr);
  if (scratch_buffer == nullptr) {
    MTL_LOG_ERROR("Could not allocate scratch buffer to build acceleration structure '%s'.",
                  name);
    [accel_struct release];
    return nil;
  }

  id<MTLAccelerationStructureCommandEncoder> enc =
      ctx.main_command_buffer.acceleration_structure_encoder_begin();
  [enc buildAccelerationStructure:accel_struct
                       descriptor:descriptor
                    scratchBuffer:scratch_buffer->get_metal_buffer()
              scratchBufferOffset:0];
  ctx.main_command_buffer.acceleration_structure_encoder_end(enc);

  scratch_buffer->free();

  return accel_struct;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bottom level acceleration structure
 * \{ */

MTLBottomLevelAS::MTLBottomLevelAS(const char *name) : BottomLevelAS(name)
{
  geometry_descriptors_ = [[NSMutableArray alloc] init];
}

MTLBottomLevelAS::~MTLBottomLevelAS()
{
  if (accel_struct_ != nil) {
    /* In-flight command buffers retain encoder referenced resources. */
    [accel_struct_ release];
    accel_struct_ = nil;
  }
  [geometry_descriptors_ release];
  geometry_descriptors_ = nil;
}

void MTLBottomLevelAS::add_geometry(IndexBuf &index_buffer_, VertBuf &vertex_buffer_)
{
  BLI_assert_msg(accel_struct_ == nil,
                 "Updating an existing acceleration structure isn't implemented.");

  MTLIndexBuf &index_buffer = static_cast<MTLIndexBuf &>(index_buffer_);
  MTLVertBuf &vertex_buffer = static_cast<MTLVertBuf &>(vertex_buffer_);

  /* Ensure GPU resident data is up to date. */
  vertex_buffer.bind();
  index_buffer.upload_data();

  const GPUVertFormat &format = vertex_buffer.format;
  /* Metal requires 3 packed 32 bit floats for vertex positions unless
   * `vertexFormat` (macOS 13+) is used. Restrict to the common position layout:
   * first attribute, at offset 0, of type float3. */
  if (format.attr_len < 1 || format.attrs[0].offset != 0 ||
      format.attrs[0].type.format != VertAttrType::SFLOAT_32_32_32)
  {
    MTL_LOG_ERROR(
        "Cannot add geometry to bottom level acceleration structure '%s': vertex position "
        "attribute must be a float3 at offset 0.",
        name_get());
    return;
  }
  /* For de-interleaved formats, the first attribute region is tightly packed. */
  const uint64_t vertex_stride = format.deinterleaved ? format.attrs[0].type.size() :
                                                        format.stride;

  id<MTLBuffer> vertex_mtl_buffer = vertex_buffer.get_metal_buffer();
  id<MTLBuffer> index_mtl_buffer = (index_buffer.ibo_ != nullptr) ?
                                       index_buffer.ibo_->get_metal_buffer() :
                                       nil;
  if (vertex_mtl_buffer == nil || index_mtl_buffer == nil) {
    MTL_LOG_ERROR(
        "Cannot add geometry to bottom level acceleration structure '%s': missing GPU buffer.",
        name_get());
    return;
  }

  const uint64_t index_size = index_buffer.is_32bit() ? 4 : 2;

  MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
      [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
  geometry.vertexBuffer = vertex_mtl_buffer;
  /* Metal has no equivalent of a base vertex (`firstVertex` in Vulkan).
   * Offsetting the vertex buffer by `index_base * stride` is equivalent since indices address
   * `vertex[index + index_base]`. */
  geometry.vertexBufferOffset = uint64_t(index_buffer.index_base_get()) * vertex_stride;
  geometry.vertexStride = vertex_stride;
  geometry.indexBuffer = index_mtl_buffer;
  geometry.indexBufferOffset = uint64_t(index_buffer.index_start_get()) * index_size;
  geometry.indexType = index_buffer.is_32bit() ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
  geometry.triangleCount = index_buffer.index_len_get() / 3;
  /* Match `VK_GEOMETRY_OPAQUE_BIT_KHR` of the reference implementation. */
  geometry.opaque = YES;

  [geometry_descriptors_ addObject:geometry];
}

void MTLBottomLevelAS::build()
{
  BLI_assert(accel_struct_ == nil);
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx != nullptr);
  if (ctx == nullptr) {
    return;
  }

  MTLPrimitiveAccelerationStructureDescriptor *descriptor =
      [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  descriptor.geometryDescriptors = geometry_descriptors_;

  accel_struct_ = mtl_acceleration_structure_build(*ctx, descriptor, name_get());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Top level acceleration structure
 * \{ */

MTLTopLevelAS::MTLTopLevelAS(const char *name) : TopLevelAS(name) {}

MTLTopLevelAS::~MTLTopLevelAS()
{
  /* Clear any stale context binding referencing this acceleration structure. */
  MTLContext *ctx = MTLContext::get();
  if (ctx != nullptr) {
    for (MTLAccelerationStructureBinding &binding : ctx->pipeline_state.accel_struct_bindings) {
      if (binding.tlas == this) {
        binding.tlas = nullptr;
        binding.bound = false;
      }
    }
  }

  if (accel_struct_ != nil) {
    /* In-flight command buffers retain encoder referenced resources. */
    [accel_struct_ release];
    accel_struct_ = nil;
  }
  if (instance_buffer_ != nullptr) {
    instance_buffer_->free();
    instance_buffer_ = nullptr;
  }
}

InstanceID MTLTopLevelAS::add_instance(const BottomLevelAS &blas,
                                       const float4x4 &mat,
                                       const uint8_t mask)
{
  BLI_assert_msg(accel_struct_ == nil,
                 "Adding instances to an existing acceleration structure isn't supported. "
                 "Updating an existing instance of an acceleration structure is supported via "
                 "the update_instance methods.");

  if (instances_.size() >= MTL_MAX_TLAS_INSTANCES) {
    MTL_LOG_ERROR(
        "Cannot add instance to top level acceleration structure as the number of "
        "instances is larger than the GPU can handle.");
    return {-1};
  }

  InstanceID instance_id = {instances_.size()};

  MTLAccelerationStructureInstanceDescriptor instance = {};
  /* `MTLPackedFloat4x3` stores 4 columns of 3 rows: the upper 3 rows of the column major
   * transform matrix. */
  instance.transformationMatrix.columns[0] = MTLPackedFloat3Make(mat.x.x, mat.x.y, mat.x.z);
  instance.transformationMatrix.columns[1] = MTLPackedFloat3Make(mat.y.x, mat.y.y, mat.y.z);
  instance.transformationMatrix.columns[2] = MTLPackedFloat3Make(mat.z.x, mat.z.y, mat.z.z);
  instance.transformationMatrix.columns[3] = MTLPackedFloat3Make(mat.w.x, mat.w.y, mat.w.z);
  /* Metal defaults to clockwise front-facing triangles. The rest of the GPU module and the
   * Vulkan reference implementation follow the counter-clockwise convention. */
  instance.options =
      MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
  instance.mask = mask;
  instance.intersectionFunctionTableOffset = 0;
  /* `accelerationStructureIndex` is assigned at build time. */
  instance.accelerationStructureIndex = 0;
  instances_.append(instance);
  blas_per_instance_.append(&unwrap(blas));
  is_dirty_ = true;

  return instance_id;
}

void MTLTopLevelAS::update_instance(InstanceID instance_id, const float4x4 &mat, uint8_t mask)
{
  BLI_assert(instance_id.id >= 0 && instance_id.id < instances_.size());
  if (instance_id.id < 0 || instance_id.id >= instances_.size()) {
    return;
  }

  MTLAccelerationStructureInstanceDescriptor &instance = instances_[instance_id.id];
  instance.transformationMatrix.columns[0] = MTLPackedFloat3Make(mat.x.x, mat.x.y, mat.x.z);
  instance.transformationMatrix.columns[1] = MTLPackedFloat3Make(mat.y.x, mat.y.y, mat.y.z);
  instance.transformationMatrix.columns[2] = MTLPackedFloat3Make(mat.z.x, mat.z.y, mat.z.z);
  instance.transformationMatrix.columns[3] = MTLPackedFloat3Make(mat.w.x, mat.w.y, mat.w.z);
  instance.mask = mask;

  is_dirty_ = true;
}

void MTLTopLevelAS::build()
{
  const bool is_built = (accel_struct_ != nil);
  if (is_built && !is_dirty_) {
    return;
  }
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx != nullptr);
  if (ctx == nullptr) {
    return;
  }

  /* Resolve referenced bottom level acceleration structures. */
  NSMutableArray<id<MTLAccelerationStructure>> *blas_array =
      [NSMutableArray arrayWithCapacity:blas_per_instance_.size()];
  for (const int64_t i : blas_per_instance_.index_range()) {
    id<MTLAccelerationStructure> blas = blas_per_instance_[i]->acceleration_structure();
    if (blas == nil) {
      MTL_LOG_ERROR(
          "Cannot add blas to top level acceleration structure '%s' as the blas has not been "
          "built.",
          name_get());
      return;
    }
    instances_[i].accelerationStructureIndex = uint32_t(i);
    [blas_array addObject:blas];
  }

  /* (Re)create the instance descriptor buffer. Freeing through the pool is deferred until
   * in-flight command buffers complete, so this is safe while a previous build is in flight. */
  if (instance_buffer_ != nullptr) {
    instance_buffer_->free();
    instance_buffer_ = nullptr;
  }
  /* A valid buffer is required even when the acceleration structure is empty. */
  MTLAccelerationStructureInstanceDescriptor empty_instance = {};
  const bool is_empty = instances_.is_empty();
  const uint64_t upload_size = uint64_t(is_empty ? 1 : instances_.size()) *
                               sizeof(MTLAccelerationStructureInstanceDescriptor);
  const void *upload_data = is_empty ? (const void *)&empty_instance :
                                       (const void *)instances_.data();
  instance_buffer_ = MTLContext::get_global_memory_manager()->allocate_with_data(
      upload_size, true, upload_data);
  BLI_assert(instance_buffer_ != nullptr);
  if (instance_buffer_ == nullptr) {
    MTL_LOG_ERROR("Unable to allocate instance buffer for top level acceleration structure '%s'.",
                  name_get());
    return;
  }

  MTLInstanceAccelerationStructureDescriptor *descriptor =
      [MTLInstanceAccelerationStructureDescriptor descriptor];
  descriptor.usage = MTLAccelerationStructureUsagePreferFastBuild;
  descriptor.instanceCount = instances_.size();
  descriptor.instanceDescriptorBuffer = instance_buffer_->get_metal_buffer();
  descriptor.instanceDescriptorBufferOffset = 0;
  descriptor.instanceDescriptorStride = sizeof(MTLAccelerationStructureInstanceDescriptor);
  descriptor.instancedAccelerationStructures = blas_array;

  if (accel_struct_ != nil) {
    /* Full rebuild. TODO: Use refitting when only instance data changed. */
    [accel_struct_ release];
    accel_struct_ = nil;
  }

  accel_struct_ = mtl_acceleration_structure_build(*ctx, descriptor, name_get());
  is_dirty_ = false;
}

void MTLTopLevelAS::bind(int slot)
{
  BLI_assert(slot >= 0 && slot < MTL_MAX_ACCELERATION_STRUCTURE_SLOTS);
  if (slot < 0 || slot >= MTL_MAX_ACCELERATION_STRUCTURE_SLOTS) {
    return;
  }
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx != nullptr);
  if (ctx == nullptr) {
    return;
  }
  MTLAccelerationStructureBinding &binding = ctx->pipeline_state.accel_struct_bindings[slot];
  binding.tlas = this;
  binding.bound = true;
}

/** \} */

}  // namespace blender::gpu
