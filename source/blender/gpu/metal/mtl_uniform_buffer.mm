/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.hh"

#include "BLI_string.hh"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"

#include "mtl_backend.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_storage_buffer.hh"
#include "mtl_uniform_buffer.hh"

namespace blender::gpu {

MTLUniformBuf::MTLUniformBuf(size_t size, const char *name) : UniformBuf(size, name) {}

MTLUniformBuf::~MTLUniformBuf()
{
  if (metal_buffer_ != nullptr) {
    metal_buffer_->free();
    metal_buffer_ = nullptr;
  }

  /* Ensure UBO is not bound to active CTX.
   * UBO bindings are reset upon Context-switch so we do not need
   * to check deactivated context's. */
  MTLContext *ctx = MTLContext::get();
  if (ctx) {
    for (MTLUniformBufferBinding &slot : ctx->pipeline_state.ubo_bindings) {
      if (slot.bound && slot.ubo == this) {
        slot.bound = false;
        slot.ubo = nullptr;
      }
    }
  }

  if (ssbo_wrapper_) {
    delete ssbo_wrapper_;
    ssbo_wrapper_ = nullptr;
  }
}

void MTLUniformBuf::update(const void *data)
{
  BLI_assert(this);
  BLI_assert(size_in_bytes_ > 0);

  /* Free existing allocation.
   * The previous UBO resource will be tracked by the memory manager,
   * in case dependent GPU work is still executing. */
  if (metal_buffer_ != nullptr) {
    metal_buffer_->free();
    metal_buffer_ = nullptr;
  }

  /* Allocate MTL buffer */
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);
  BLI_assert(ctx->device);
  UNUSED_VARS_NDEBUG(ctx);

  if (data) {
    metal_buffer_ = MTLContext::get_global_memory_manager()->allocate_with_data(
        size_in_bytes_, true, data);
  }
  else {
    metal_buffer_ = MTLContext::get_global_memory_manager()->allocate(size_in_bytes_, true);
  }

#ifndef NDEBUG
  static std::atomic<int> global_counter = 0;
  int index = global_counter.fetch_add(1);
  metal_buffer_->set_label([NSString stringWithFormat:@"UBO %i %s", index, name_]);
#endif

  BLI_assert(metal_buffer_ != nullptr);
  BLI_assert(metal_buffer_->get_metal_buffer() != nil);
}

/* Kill-switch: forces clear_to_zero() to always use the original calloc()+update()+free() CPU
 * path below, matching pre-optimization behavior exactly. Checked once and cached, matching the
 * `METAL_FORCE_INTEL` / `BLENDER_METAL_RAYTRACING` pattern in mtl_backend.mm. */
static bool metal_ubo_cpu_clear_enabled()
{
  static const bool cpu_clear = (getenv("BLENDER_METAL_UBO_CPU_CLEAR") != nullptr);
  return cpu_clear;
}

void MTLUniformBuf::clear_to_zero()
{
  MTLContext *ctx = MTLContext::get();

  /* GPU-side clear: fill the *existing* allocation in place via a blit encoder instead of the
   * calloc()+update()+free() round trip below. This is safe without reallocating (unlike
   * update()'s CPU-upload path, which must orphan the old buffer because a host memcpy racing a
   * still-executing GPU read would be undefined): `fillBuffer:` is itself a GPU command recorded
   * into the command stream, so Metal's automatic hazard tracking orders it after every
   * previously-encoded GPU access to this buffer (including reads from commands already
   * submitted on this serial queue) and before anything encoded afterwards -- there is no
   * CPU/GPU race to guard against. This mirrors the already-shipping in-place
   * `MTLStorageBuf::clear()` (mtl_storage_buffer.mm), which fills a live SSBO allocation via the
   * exact same `[blit_encoder fillBuffer:range:value:]` call for the equal-clear-byte case that
   * value 0 always satisfies.
   * We avoid this path if a render pass is currently active: ending it only to insert a blit
   * encoder would be the TBDR load/store round-trip anti-pattern this optimization round is
   * about avoiding elsewhere (see mtl_framebuffer.mm clear deferral) -- the CPU path below needs
   * no encoder at all, so it is strictly cheaper in that specific case. */
  if (!metal_ubo_cpu_clear_enabled() && ctx && !ctx->main_command_buffer.is_inside_render_pass()) {
    if (metal_buffer_ == nullptr) {
      /* Ensure a correctly-sized allocation exists. Contents are undefined until the fillBuffer
       * below runs; this mirrors the allocate-only fallback already used by bind(). */
      this->update(nullptr);
    }
    id<MTLBuffer> mtl_buf = metal_buffer_->get_metal_buffer();
    BLI_assert(mtl_buf != nil);

    id<MTLBlitCommandEncoder> blit_encoder = ctx->main_command_buffer.ensure_begin_blit_encoder();
    [blit_encoder fillBuffer:mtl_buf range:NSMakeRange(0, size_in_bytes_) value:0];
    return;
  }

  /* Fallback: kill-switch set, no active context, or a render pass is currently active. */
  void *clear_data = calloc(1, size_in_bytes_);
  this->update(clear_data);
  free(clear_data);
}

void MTLUniformBuf::bind(int slot)
{
  if (slot < 0) {
    MTL_LOG_WARNING("Failed to bind UBO %p. uniform location %d invalid.", this, slot);
    return;
  }

  BLI_assert(slot < MTL_MAX_BUFFER_BINDINGS);

  /* Bind current UBO to active context. */
  MTLContext *ctx = MTLContext::get();
  BLI_assert(ctx);

  MTLUniformBufferBinding &ctx_ubo_bind_slot = ctx->pipeline_state.ubo_bindings[slot];
  ctx_ubo_bind_slot.ubo = this;
  ctx_ubo_bind_slot.bound = true;

  bind_slot_ = slot;
  bound_ctx_ = ctx;

  /* Check if we have any deferred data to upload. */
  if (data_ != nullptr) {
    this->update(data_);
    MEM_SAFE_DELETE_VOID(data_);
  }

  /* Ensure there is at least an empty dummy buffer. */
  if (metal_buffer_ == nullptr) {
    this->update(nullptr);
  }
}

void MTLUniformBuf::bind_as_ssbo(int slot)
{
  if (slot < 0) {
    MTL_LOG_WARNING("Failed to bind UBO %p as SSBO. uniform location %d invalid.", this, slot);
    return;
  }

  /* We need to ensure data is actually allocated if using as an SSBO, as resource may be written
   * to. */
  if (metal_buffer_ == nullptr) {
    /* Check if we have any deferred data to upload. */
    if (data_ != nullptr) {
      this->update(data_);
      MEM_SAFE_DELETE_VOID(data_);
    }
    else {
      this->clear_to_zero();
    }
  }

  /* Create MTLStorageBuffer to wrap this resource and use conventional binding. */
  if (ssbo_wrapper_ == nullptr) {
    ssbo_wrapper_ = new MTLStorageBuf(this, size_in_bytes_);
  }

  ssbo_wrapper_->bind(slot);
}

void MTLUniformBuf::unbind()
{
  /* Unbind in debug mode to validate missing binds.
   * Otherwise, only perform a full unbind upon destruction
   * to ensure no lingering references. */
#ifndef NDEBUG
  if (true)
#else
  if (G.debug & G_DEBUG_GPU)
#endif
  {
    if (bound_ctx_ != nullptr && bind_slot_ > -1) {
      MTLUniformBufferBinding &ctx_ubo_bind_slot =
          bound_ctx_->pipeline_state.ubo_bindings[bind_slot_];
      if (ctx_ubo_bind_slot.bound && ctx_ubo_bind_slot.ubo == this) {
        ctx_ubo_bind_slot.bound = false;
        ctx_ubo_bind_slot.ubo = nullptr;
      }
    }
  }

  /* Reset bind index. */
  bind_slot_ = -1;
  bound_ctx_ = nullptr;
}

id<MTLBuffer> MTLUniformBuf::get_metal_buffer()
{
  BLI_assert(this);
  if (metal_buffer_ != nullptr) {
    metal_buffer_->debug_ensure_used();
    return metal_buffer_->get_metal_buffer();
  }
  return nil;
}

size_t MTLUniformBuf::get_size()
{
  BLI_assert(this);
  return size_in_bytes_;
}

}  // namespace blender::gpu
