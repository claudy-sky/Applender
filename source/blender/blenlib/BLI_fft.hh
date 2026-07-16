/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Minimal 2D real FFT wrapper for the compositor, following FFTW conventions:
 *
 * - Transforms are unnormalized: a forward transform followed by a backward transform scales the
 *   data by the product of the transform dimensions.
 * - Real data is stored row-major as `size.y` rows of `size.x` contiguous floats.
 * - Complex data is the Hermitian-reduced half spectrum: `size.y` rows of `size.x / 2 + 1`
 *   interleaved complex values, reduced along the x axis. See Section 4.3.4 Real-data DFT Array
 *   Format in the FFTW manual.
 *
 * The default implementation is FFTW and behaves exactly like calling FFTW directly. When built
 * with WITH_FFT_ACCELERATE on Apple platforms, transforms whose sizes are supported by Apple's
 * vDSP DFT are executed through the Accelerate framework instead, with layout and scaling
 * converted to match the FFTW conventions above. Setting the environment variable
 * BLENDER_FFT_FORCE_FFTW to a non-zero value restores pure FFTW at runtime.
 *
 * This API is only available when building with WITH_FFTW3.
 */

#include <complex>
#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_utility_mixins.hh"

namespace blender::fft {

/**
 * Real to complex and complex to real transforms are more efficient when their input has a
 * specific size. This function finds the most optimal size that is more than or equal the given
 * size for the active FFT backend. The input data can then be zero padded to the optimal size for
 * better performance. For the 2D variant, the x component is the size of the contiguous
 * Hermitian-reduced axis of a 2D real transform and the y component is the other axis.
 */
int optimal_size_for_real_transform(int size);
int2 optimal_size_for_real_transform(int2 size);

/**
 * Allocate buffers suitably aligned for either backend. Buffers allocated by these functions must
 * be freed with #free_buffer.
 */
float *alloc_real(int64_t count);
std::complex<float> *alloc_complex(int64_t count);
void free_buffer(void *buffer);

/**
 * A 2D real to complex forward transform plan for the given size, where `size.x` is the length of
 * the contiguous rows and `size.y` is the number of rows. The given buffers are exemplars used
 * for planning only and their contents need not be initialized, but they should have the same
 * alignment as the buffers that will be used for execution, which is guaranteed if all buffers
 * are allocated using #alloc_real and #alloc_complex. Plans should be created before initializing
 * the content of the buffers, since planning may overwrite them.
 */
class RealToComplex2DPlan : NonCopyable, NonMovable {
 private:
  struct Impl;
  Impl *impl_;

 public:
  RealToComplex2DPlan(int2 size, float *real_buffer, std::complex<float> *complex_buffer);
  ~RealToComplex2DPlan();

  /**
   * Transform the given real input into the given complex half spectrum output. The buffers need
   * not be the ones given at construction, but must have the same size and alignment. This method
   * is thread-safe and may be called concurrently on different buffers.
   */
  void execute(float *input, std::complex<float> *output) const;
};

/**
 * A 2D complex to real backward transform plan, the inverse of #RealToComplex2DPlan. See its
 * documentation for the buffer requirements.
 */
class ComplexToReal2DPlan : NonCopyable, NonMovable {
 private:
  struct Impl;
  Impl *impl_;

 public:
  ComplexToReal2DPlan(int2 size, std::complex<float> *complex_buffer, float *real_buffer);
  ~ComplexToReal2DPlan();

  /**
   * Transform the given complex half spectrum input into the given real output. The contents of
   * the input buffer are undefined after the call, matching FFTW's complex to real transforms.
   * This method is thread-safe and may be called concurrently on different buffers.
   */
  void execute(std::complex<float> *input, float *output) const;
};

}  // namespace blender::fft
