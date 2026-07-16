/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(WITH_FFTW3)
#  include <fftw3.h>
#endif

#if defined(WITH_FFT_ACCELERATE) && defined(__APPLE__)
#  define BLI_FFT_USE_VDSP
#  include <Accelerate/Accelerate.h>
#endif

#include "MEM_guardedalloc.h"

#include "BLI_fft.hh"
#include "BLI_fftw.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.hh"

namespace blender::fft {

#if defined(BLI_FFT_USE_VDSP)

static bool use_vdsp_backend()
{
  static const bool use_vdsp = []() {
    const char *env = std::getenv("BLENDER_FFT_FORCE_FFTW");
    return !(env && env[0] != '\0' && env[0] != '0');
  }();
  return use_vdsp;
}

/* Serializes vDSP DFT setup creation and destruction: setups may share internal data structures
 * and Apple only documents execution as safe for concurrent use, not setup management. */
static std::mutex &vdsp_setup_mutex()
{
  static std::mutex mutex;
  return mutex;
}

/* vdsp-core-begin: everything until vdsp-core-end is freestanding except for the vDSP API,
 * threading::parallel_for and IndexRange, so it can be extracted and exercised on any host
 * against a mock vDSP implementing the documented API contract. */

/* vDSP_DFT_zop_CreateSetup and vDSP_DFT_zrop_CreateSetup only support transform lengths of the
 * form f * 2^n with f in {1, 3, 5, 15}, where n >= 3 for complex transforms (zop) and n >= 4 for
 * real transforms (zrop). */
static constexpr int VDSP_REAL_MIN_TWO_EXPONENT = 4;
static constexpr int VDSP_COMPLEX_MIN_TWO_EXPONENT = 3;

static bool is_vdsp_dft_size(const int size, const int min_two_exponent)
{
  if (size <= 0) {
    return false;
  }
  int odd_part = size;
  int two_exponent = 0;
  while (odd_part % 2 == 0) {
    odd_part /= 2;
    two_exponent++;
  }
  if (two_exponent < min_two_exponent) {
    return false;
  }
  return odd_part == 1 || odd_part == 3 || odd_part == 5 || odd_part == 15;
}

static int next_vdsp_dft_size(const int size, const int min_two_exponent)
{
  int64_t best = INT64_MAX;
  for (const int64_t factor : {1, 3, 5, 15}) {
    int64_t candidate = factor << min_two_exponent;
    while (candidate < size) {
      candidate *= 2;
    }
    best = std::min(best, candidate);
  }
  return int(best);
}

/* Transforms the given `ny * nx` row-major real input into its `ny * (nx / 2 + 1)` row-major
 * Hermitian-reduced half spectrum following FFTW's conventions, using a 1D real DFT along the
 * rows followed by a 1D complex DFT along the columns. `row_setup` must be a forward
 * vDSP_DFT_zrop setup of length `nx` and `column_setup` a forward vDSP_DFT_zop setup of length
 * `ny`. */
static void vdsp_execute_forward_2d(vDSP_DFT_Setup row_setup,
                                    vDSP_DFT_Setup column_setup,
                                    const int nx,
                                    const int ny,
                                    const float *input,
                                    std::complex<float> *output)
{
  const int nxc = nx / 2 + 1;
  const int half_nx = nx / 2;
  const int64_t plane_size = int64_t(nxc) * ny;

  /* Split complex planes holding the intermediate spectrum, `a` starts in row-major layout with
   * `ny` rows of `nxc` values, `b` holds the transposed layout. */
  std::vector<float> real_a(plane_size);
  std::vector<float> imag_a(plane_size);
  std::vector<float> real_b(plane_size);
  std::vector<float> imag_b(plane_size);

  /* 1D real forward DFT of every row. */
  threading::parallel_for(IndexRange(ny), 8, [&](const IndexRange range) {
    std::vector<float> split_input(nx);
    std::vector<float> packed_output(nx);
    DSPSplitComplex split = {split_input.data(), split_input.data() + half_nx};
    float *packed_real = packed_output.data();
    float *packed_imag = packed_output.data() + half_nx;
    for (const int64_t y : range) {
      /* De-interleave the row into even/odd halves, the input layout of vDSP real DFTs. */
      vDSP_ctoz(reinterpret_cast<const DSPComplex *>(input + y * nx), 2, &split, 1, half_nx);
      vDSP_DFT_Execute(row_setup, split.realp, split.imagp, packed_real, packed_imag);
      /* Convert to FFTW conventions: the output of vDSP's real forward DFT is scaled by 2
       * relative to the mathematical DFT and packs the purely real Nyquist coefficient into the
       * imaginary slot of the purely real DC element. */
      float *row_real = real_a.data() + y * nxc;
      float *row_imag = imag_a.data() + y * nxc;
      row_real[0] = packed_real[0] * 0.5f;
      row_imag[0] = 0.0f;
      for (int x = 1; x < half_nx; x++) {
        row_real[x] = packed_real[x] * 0.5f;
        row_imag[x] = packed_imag[x] * 0.5f;
      }
      row_real[half_nx] = packed_imag[0] * 0.5f;
      row_imag[half_nx] = 0.0f;
    }
  });

  /* Transpose the (ny x nxc) planes to (nxc x ny) so that columns become contiguous. */
  vDSP_mtrans(real_a.data(), 1, real_b.data(), 1, vDSP_Length(nxc), vDSP_Length(ny));
  vDSP_mtrans(imag_a.data(), 1, imag_b.data(), 1, vDSP_Length(nxc), vDSP_Length(ny));

  /* 1D complex forward DFT along every column. vDSP complex DFTs are unnormalized with a negative
   * exponent in the forward direction, exactly like FFTW, so no scaling is needed. */
  threading::parallel_for(IndexRange(nxc), 4, [&](const IndexRange range) {
    for (const int64_t x : range) {
      const int64_t offset = x * ny;
      vDSP_DFT_Execute(column_setup,
                       real_b.data() + offset,
                       imag_b.data() + offset,
                       real_a.data() + offset,
                       imag_a.data() + offset);
    }
  });

  /* Transpose back to row-major and interleave into the output. */
  vDSP_mtrans(real_a.data(), 1, real_b.data(), 1, vDSP_Length(ny), vDSP_Length(nxc));
  vDSP_mtrans(imag_a.data(), 1, imag_b.data(), 1, vDSP_Length(ny), vDSP_Length(nxc));
  DSPSplitComplex interleave_input = {real_b.data(), imag_b.data()};
  vDSP_ztoc(&interleave_input, 1, reinterpret_cast<DSPComplex *>(output), 2, plane_size);
}

/* The inverse of vdsp_execute_forward_2d, following FFTW's conventions for complex to real
 * transforms: unnormalized, so the round trip scales by `nx * ny`, and the imaginary parts of the
 * DC and Nyquist elements of each row are assumed to be zero. The input is not modified, which is
 * a superset of FFTW's contract of leaving the input in an undefined state. `row_setup` must be
 * an inverse vDSP_DFT_zrop setup of length `nx` and `column_setup` an inverse vDSP_DFT_zop setup
 * of length `ny`. */
static void vdsp_execute_backward_2d(vDSP_DFT_Setup row_setup,
                                     vDSP_DFT_Setup column_setup,
                                     const int nx,
                                     const int ny,
                                     const std::complex<float> *input,
                                     float *output)
{
  const int nxc = nx / 2 + 1;
  const int half_nx = nx / 2;
  const int64_t plane_size = int64_t(nxc) * ny;

  std::vector<float> real_a(plane_size);
  std::vector<float> imag_a(plane_size);
  std::vector<float> real_b(plane_size);
  std::vector<float> imag_b(plane_size);

  /* De-interleave the half spectrum into split complex planes. */
  DSPSplitComplex deinterleave_output = {real_a.data(), imag_a.data()};
  vDSP_ctoz(
      reinterpret_cast<const DSPComplex *>(input), 2, &deinterleave_output, 1, plane_size);

  /* Transpose the (ny x nxc) planes to (nxc x ny) so that columns become contiguous. */
  vDSP_mtrans(real_a.data(), 1, real_b.data(), 1, vDSP_Length(nxc), vDSP_Length(ny));
  vDSP_mtrans(imag_a.data(), 1, imag_b.data(), 1, vDSP_Length(nxc), vDSP_Length(ny));

  /* 1D complex inverse DFT along every column, unnormalized with a positive exponent like
   * FFTW's backward transforms. */
  threading::parallel_for(IndexRange(nxc), 4, [&](const IndexRange range) {
    for (const int64_t x : range) {
      const int64_t offset = x * ny;
      vDSP_DFT_Execute(column_setup,
                       real_b.data() + offset,
                       imag_b.data() + offset,
                       real_a.data() + offset,
                       imag_a.data() + offset);
    }
  });

  /* Transpose back to row-major. */
  vDSP_mtrans(real_a.data(), 1, real_b.data(), 1, vDSP_Length(ny), vDSP_Length(nxc));
  vDSP_mtrans(imag_a.data(), 1, imag_b.data(), 1, vDSP_Length(ny), vDSP_Length(nxc));

  /* 1D real inverse DFT of every row. */
  threading::parallel_for(IndexRange(ny), 8, [&](const IndexRange range) {
    std::vector<float> packed_input(nx);
    std::vector<float> split_output(nx);
    float *packed_real = packed_input.data();
    float *packed_imag = packed_input.data() + half_nx;
    DSPSplitComplex split = {split_output.data(), split_output.data() + half_nx};
    for (const int64_t y : range) {
      const float *row_real = real_b.data() + y * nxc;
      const float *row_imag = imag_b.data() + y * nxc;
      /* Pack into the vDSP layout: the real part of the Nyquist element goes into the imaginary
       * slot of the DC element, while the imaginary parts of both are assumed zero. The inverse
       * real DFT then computes the unnormalized mathematical inverse, like FFTW. */
      packed_real[0] = row_real[0];
      packed_imag[0] = row_real[half_nx];
      for (int x = 1; x < half_nx; x++) {
        packed_real[x] = row_real[x];
        packed_imag[x] = row_imag[x];
      }
      vDSP_DFT_Execute(row_setup, packed_real, packed_imag, split.realp, split.imagp);
      /* Interleave the even/odd output halves back into a contiguous row. */
      vDSP_ztoc(&split, 1, reinterpret_cast<DSPComplex *>(output + y * nx), 2, half_nx);
    }
  });
}

/* vdsp-core-end. */

#endif /* BLI_FFT_USE_VDSP */

int optimal_size_for_real_transform(const int size)
{
#if defined(BLI_FFT_USE_VDSP)
  if (use_vdsp_backend()) {
    return next_vdsp_dft_size(size, VDSP_REAL_MIN_TWO_EXPONENT);
  }
#endif
  return fftw::optimal_size_for_real_transform(size);
}

int2 optimal_size_for_real_transform(const int2 size)
{
#if defined(BLI_FFT_USE_VDSP)
  if (use_vdsp_backend()) {
    return int2(next_vdsp_dft_size(size.x, VDSP_REAL_MIN_TWO_EXPONENT),
                next_vdsp_dft_size(size.y, VDSP_COMPLEX_MIN_TWO_EXPONENT));
  }
#endif
  return fftw::optimal_size_for_real_transform(size);
}

#if defined(WITH_FFTW3)

float *alloc_real(const int64_t count)
{
  return fftwf_alloc_real(count);
}

std::complex<float> *alloc_complex(const int64_t count)
{
  return reinterpret_cast<std::complex<float> *>(fftwf_alloc_complex(count));
}

void free_buffer(void *buffer)
{
  fftwf_free(buffer);
}

/* Returns true if vDSP should be used for a 2D real transform of the given size, in which case
 * the size is guaranteed to be supported by the vDSP DFT setup functions. Sizes computed by
 * optimal_size_for_real_transform always are, but arbitrary sizes fall back to FFTW. */
static bool should_use_vdsp_for_size(const int2 size)
{
#  if defined(BLI_FFT_USE_VDSP)
  return use_vdsp_backend() && is_vdsp_dft_size(size.x, VDSP_REAL_MIN_TWO_EXPONENT) &&
         is_vdsp_dft_size(size.y, VDSP_COMPLEX_MIN_TWO_EXPONENT);
#  else
  UNUSED_VARS(size);
  return false;
#  endif
}

struct RealToComplex2DPlan::Impl {
  int2 size = int2(0);
#  if defined(BLI_FFT_USE_VDSP)
  vDSP_DFT_Setup row_setup = nullptr;
  vDSP_DFT_Setup column_setup = nullptr;
#  endif
  fftwf_plan plan = nullptr;
};

RealToComplex2DPlan::RealToComplex2DPlan(const int2 size,
                                         float *real_buffer,
                                         std::complex<float> *complex_buffer)
{
  impl_ = MEM_new<Impl>(__func__);
  impl_->size = size;

  if (should_use_vdsp_for_size(size)) {
#  if defined(BLI_FFT_USE_VDSP)
    std::lock_guard lock(vdsp_setup_mutex());
    impl_->row_setup = vDSP_DFT_zrop_CreateSetup(nullptr, vDSP_Length(size.x), vDSP_DFT_FORWARD);
    impl_->column_setup = vDSP_DFT_zop_CreateSetup(nullptr, vDSP_Length(size.y), vDSP_DFT_FORWARD);
    if (impl_->row_setup && impl_->column_setup) {
      return;
    }
    /* Setup creation failed, destroy any partial setup and fall back to FFTW. */
    if (impl_->row_setup) {
      vDSP_DFT_DestroySetup(impl_->row_setup);
      impl_->row_setup = nullptr;
    }
    if (impl_->column_setup) {
      vDSP_DFT_DestroySetup(impl_->column_setup);
      impl_->column_setup = nullptr;
    }
#  endif
  }

  impl_->plan = fftwf_plan_dft_r2c_2d(size.y,
                                      size.x,
                                      real_buffer,
                                      reinterpret_cast<fftwf_complex *>(complex_buffer),
                                      FFTW_ESTIMATE);
}

RealToComplex2DPlan::~RealToComplex2DPlan()
{
#  if defined(BLI_FFT_USE_VDSP)
  if (impl_->row_setup || impl_->column_setup) {
    std::lock_guard lock(vdsp_setup_mutex());
    if (impl_->row_setup) {
      vDSP_DFT_DestroySetup(impl_->row_setup);
    }
    if (impl_->column_setup) {
      vDSP_DFT_DestroySetup(impl_->column_setup);
    }
  }
#  endif
  if (impl_->plan) {
    fftwf_destroy_plan(impl_->plan);
  }
  MEM_delete(impl_);
}

void RealToComplex2DPlan::execute(float *input, std::complex<float> *output) const
{
#  if defined(BLI_FFT_USE_VDSP)
  if (impl_->row_setup) {
    vdsp_execute_forward_2d(
        impl_->row_setup, impl_->column_setup, impl_->size.x, impl_->size.y, input, output);
    return;
  }
#  endif
  fftwf_execute_dft_r2c(impl_->plan, input, reinterpret_cast<fftwf_complex *>(output));
}

struct ComplexToReal2DPlan::Impl {
  int2 size = int2(0);
#  if defined(BLI_FFT_USE_VDSP)
  vDSP_DFT_Setup row_setup = nullptr;
  vDSP_DFT_Setup column_setup = nullptr;
#  endif
  fftwf_plan plan = nullptr;
};

ComplexToReal2DPlan::ComplexToReal2DPlan(const int2 size,
                                         std::complex<float> *complex_buffer,
                                         float *real_buffer)
{
  impl_ = MEM_new<Impl>(__func__);
  impl_->size = size;

  if (should_use_vdsp_for_size(size)) {
#  if defined(BLI_FFT_USE_VDSP)
    std::lock_guard lock(vdsp_setup_mutex());
    impl_->row_setup = vDSP_DFT_zrop_CreateSetup(nullptr, vDSP_Length(size.x), vDSP_DFT_INVERSE);
    impl_->column_setup = vDSP_DFT_zop_CreateSetup(nullptr, vDSP_Length(size.y), vDSP_DFT_INVERSE);
    if (impl_->row_setup && impl_->column_setup) {
      return;
    }
    /* Setup creation failed, destroy any partial setup and fall back to FFTW. */
    if (impl_->row_setup) {
      vDSP_DFT_DestroySetup(impl_->row_setup);
      impl_->row_setup = nullptr;
    }
    if (impl_->column_setup) {
      vDSP_DFT_DestroySetup(impl_->column_setup);
      impl_->column_setup = nullptr;
    }
#  endif
  }

  impl_->plan = fftwf_plan_dft_c2r_2d(size.y,
                                      size.x,
                                      reinterpret_cast<fftwf_complex *>(complex_buffer),
                                      real_buffer,
                                      FFTW_ESTIMATE);
}

ComplexToReal2DPlan::~ComplexToReal2DPlan()
{
#  if defined(BLI_FFT_USE_VDSP)
  if (impl_->row_setup || impl_->column_setup) {
    std::lock_guard lock(vdsp_setup_mutex());
    if (impl_->row_setup) {
      vDSP_DFT_DestroySetup(impl_->row_setup);
    }
    if (impl_->column_setup) {
      vDSP_DFT_DestroySetup(impl_->column_setup);
    }
  }
#  endif
  if (impl_->plan) {
    fftwf_destroy_plan(impl_->plan);
  }
  MEM_delete(impl_);
}

void ComplexToReal2DPlan::execute(std::complex<float> *input, float *output) const
{
#  if defined(BLI_FFT_USE_VDSP)
  if (impl_->row_setup) {
    vdsp_execute_backward_2d(
        impl_->row_setup, impl_->column_setup, impl_->size.x, impl_->size.y, input, output);
    return;
  }
#  endif
  fftwf_execute_dft_c2r(impl_->plan, reinterpret_cast<fftwf_complex *>(input), output);
}

#endif /* WITH_FFTW3 */

}  // namespace blender::fft
