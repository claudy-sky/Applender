/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <mutex>
#include <string>

#include "BLI_vector.hh"

#include "gpu_backend.hh"
#include "gpu_shader_private.hh"
#include "mtl_capabilities.hh"

namespace blender::gpu {

class Batch;
class FrameBuffer;
class QueryPool;
class Shader;
class UniformBuf;
class VertBuf;
class MTLContext;

class MTLBackend : public GPUBackend {
  friend class MTLContext;

 public:
  /* Capabilities. */
  static MTLCapabilities capabilities;

  static MTLCapabilities &get_capabilities()
  {
    return MTLBackend::capabilities;
  }

  ~MTLBackend() override
  {
    MTLBackend::platform_exit();
  }

  void init_resources() override;

  void delete_resources() override;

  static bool metal_is_supported();
  static MTLBackend *get()
  {
    return static_cast<MTLBackend *>(GPUBackend::get());
  }

  void compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len) override;
  void compute_dispatch_indirect(StorageBuf *indirect_buf) override;

  /* MTL Allocators need to be implemented in separate `.mm` files,
   * due to allocation of Objective-C objects. */
  Context *context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context) override;
  Batch *batch_alloc() override;
  Fence *fence_alloc() override;
  FrameBuffer *framebuffer_alloc(const char *name) override;
  IndexBuf *indexbuf_alloc() override;
  PixelBuffer *pixelbuf_alloc(size_t size) override;
  QueryPool *querypool_alloc() override;
  Shader *shader_alloc(const char *name) override;
  Texture *texture_alloc(const char *name) override;
  TexturePool *texturepool_alloc() override;
  UniformBuf *uniformbuf_alloc(size_t size, const char *name) override;
  StorageBuf *storagebuf_alloc(size_t size, GPUUsageType usage, const char *name) override;
  VertBuf *vertbuf_alloc() override;
  TopLevelAS *tlas_alloc(const char *name) override;
  BottomLevelAS *blas_alloc(const char *name) override;
  void shader_cache_dir_clear_old() override;

  /* Render Frame Coordination. */
  void render_begin() override;
  void render_end() override;
  void render_step(bool force_resource_release = false) override;
  bool is_inside_render_boundary();

  /* Opaque accessor for the shared on-disk PSO binary archive (opt-in, see
   * `pso_binary_archive_` below and mtl_backend.mm for the full design). Returns nullptr if
   * `BLENDER_METAL_PSO_ARCHIVE` is unset or initialization failed.
   * Typed as `void *` rather than `id<MTLBinaryArchive>` because this header is also pulled into
   * plain C++ (non-Objective-C) translation units (mtl_shader_generate.cc, gpu_context.cc) --
   * unlike mtl_context.hh, it cannot assume an Objective-C++ compiler. Callers in `.mm` files
   * cast the result to `id<MTLBinaryArchive>`, mirroring how `GHOST_ContextMTL::metalDevice()`
   * hands back an opaque `MTLDevice *` for the same reason. */
  void *get_pso_binary_archive_raw()
  {
    return pso_binary_archive_;
  }

  /* Guards mutation (`add{Render,Compute}PipelineFunctionsWithDescriptor:`) of the archive
   * returned by `get_pso_binary_archive_raw()`. PSO bakes can run concurrently on
   * `MTLShaderCompiler` worker threads and `MTLBinaryArchive`'s mutators are not documented as
   * thread-safe, so every harvest call site must hold this lock for the duration of the add*
   * call (and nothing more -- keep the critical section tiny). */
  std::mutex &get_pso_binary_archive_mutex()
  {
    return pso_binary_archive_mutex_;
  }

 private:
  static void platform_init(MTLContext *ctx);
  static void platform_exit();

  static void capabilities_init(MTLContext *ctx);

  /* --- Opt-in on-disk MTLBinaryArchive PSO cache ----------------------------------------
   * Kill-switch: BLENDER_METAL_PSO_ARCHIVE=1, default OFF. macOS already maintains its own
   * system-wide Metal shader cache, but that cache is opaque and evictable and cannot guarantee
   * a warm start. This explicit archive exists purely to give *deterministic* warm starts for
   * measurement, and stays opt-in until profiling justifies making it the default.
   *
   * Owned by the backend singleton rather than per-`MTLContext`: `MTLShaderCompiler` spins up a
   * secondary `MTLContext` per worker thread (see `GPUWorker::ContextType::PerThread`), so an
   * archive owned by `MTLContext` would be loaded/serialized redundantly -- and raced -- by every
   * compiler thread. `MTLBackend` is a single process-wide instance, matching the single on-disk
   * archive file.
   *
   * See `pso_binary_archive_init()` / `pso_binary_archive_serialize_and_free()` in
   * mtl_backend.mm for load/serialize, and the two `bake_*_pipeline_state()` functions in
   * mtl_shader.mm for the harvest/lookup call sites. */
  void *pso_binary_archive_ = nullptr;
  std::mutex pso_binary_archive_mutex_;
  /* Absolute path resolved once by `pso_binary_archive_init()`; empty if the feature is disabled
   * or the path could not be resolved. Kept so `pso_binary_archive_serialize_and_free()` can
   * write back to the same location without recomputing it. */
  std::string pso_binary_archive_path_;

  void pso_binary_archive_init();
  void pso_binary_archive_serialize_and_free();
};

}  // namespace blender::gpu
