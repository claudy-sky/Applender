/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Bit-exact parity tests for the NEON pixel-conversion fast paths in `conversion.cc` and
 * `filter.cc` (Apple Silicon / Metal-only build). Each test calls the public conversion
 * function -- which internally dispatches to NEON batches plus a scalar tail on
 * `__ARM_NEON` builds, and to the scalar loop only otherwise -- and compares its output,
 * element-for-element, against Blender's scalar reference functions from
 * `BLI_math_color_c.hh` / `BLI_math_base_c.hh` (the very same functions the scalar tail
 * loop calls). On an `__ARM_NEON` build this is a genuine NEON-vs-scalar-reference
 * bit-exactness check; on other platforms the production code has no NEON branch to take,
 * so the same assertions instead validate scalar self-consistency (see
 * `NeonPixelConversionTest.platform_note` below).
 */

#include "testing/testing.h"

#include "BLI_math_base_c.hh"
#include "BLI_math_color_c.hh"

#include "IMB_imbuf.hh"

#include "intern/IMB_filter.hh"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

/* The scalar reference calls in this test must round exactly like the
 * production scalar path: keep FMA contraction off in this TU as well. */
#if defined(__clang__)
#  pragma clang fp contract(off)
#endif

namespace blender::imbuf::tests {

using uchar = unsigned char;

TEST(NeonPixelConversionTest, platform_note)
{
#if defined(__ARM_NEON)
  GTEST_LOG_(INFO) << "__ARM_NEON is defined: the tests below exercise the real NEON batch "
                       "paths (plus their scalar tails) and compare against the scalar "
                       "reference functions -- a genuine bit-exactness check.";
#else
  GTEST_LOG_(INFO) << "__ARM_NEON is not defined on this platform: the production code has "
                       "no NEON branch to take, so the tests below run the scalar-only path "
                       "and validate it is self-consistent with the scalar reference "
                       "functions (the parity proof for the NEON path itself is the "
                       "aarch64-cross-compiled model test, not this binary).";
#endif
  SUCCEED();
}

/* -------------------------------------------------------------------- */
/** \name IMB_buffer_float_from_byte (byte -> float)
 * \{ */

TEST(NeonPixelConversionTest, ByteToFloat_ExhaustiveAllChannelValues)
{
  /* Width is a multiple of 4 (NEON batch size for this conversion) so the whole row is
   * covered by full batches on an __ARM_NEON build; every one of the 256 possible uchar
   * values appears in every channel slot. */
  constexpr int width = 256;
  std::vector<uchar> src(size_t(width) * 4);
  for (int i = 0; i < width; i++) {
    src[i * 4 + 0] = uchar(i);
    src[i * 4 + 1] = uchar(i);
    src[i * 4 + 2] = uchar(i);
    src[i * 4 + 3] = uchar(i);
  }
  std::vector<float> dst(size_t(width) * 4, -1.0f);

  IMB_buffer_float_from_byte(dst.data(), src.data(), width, 1, width, width);

  for (int i = 0; i < width; i++) {
    float expected[4];
    rgba_uchar_to_float(expected, &src[i * 4]);
    for (int c = 0; c < 4; c++) {
      EXPECT_EQ(dst[i * 4 + c], expected[c]) << "pixel " << i << " channel " << c;
    }
  }
}

TEST(NeonPixelConversionTest, ByteToFloat_RemainderTailWidths)
{
  /* Widths that are NOT multiples of 4, to exercise the NEON-batch + scalar-tail boundary
   * (and, on non-ARM, the scalar-only loop over an odd width). */
  for (int width : {1, 2, 3, 5, 7, 15, 17, 61, 253, 255, 257}) {
    std::vector<uchar> src(size_t(width) * 4);
    for (int i = 0; i < width * 4; i++) {
      src[i] = uchar((i * 37 + 11) & 0xFF);
    }
    std::vector<float> dst(size_t(width) * 4, -1.0f);

    IMB_buffer_float_from_byte(dst.data(), src.data(), width, 1, width, width);

    for (int i = 0; i < width; i++) {
      float expected[4];
      rgba_uchar_to_float(expected, &src[i * 4]);
      for (int c = 0; c < 4; c++) {
        ASSERT_EQ(dst[i * 4 + c], expected[c])
            << "width " << width << " pixel " << i << " channel " << c;
      }
    }
  }
}

TEST(NeonPixelConversionTest, ByteToFloat_MultiRowStrided)
{
  /* dest_stride != width and src_stride != width, matching how IMB_float_from_byte_ex()
   * calls this for a sub-region of a larger image. */
  constexpr int width = 37;
  constexpr int height = 5;
  constexpr int src_stride = 64;
  constexpr int dest_stride = 48;
  std::vector<uchar> src(size_t(src_stride) * height * 4, 0);
  std::vector<float> dst(size_t(dest_stride) * height * 4, -1.0f);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uchar *px = &src[(size_t(src_stride) * y + x) * 4];
      px[0] = uchar((x * 5 + y * 13) & 0xFF);
      px[1] = uchar((x * 7 + y * 17) & 0xFF);
      px[2] = uchar((x * 11 + y * 19) & 0xFF);
      px[3] = uchar((x * 3 + y * 23) & 0xFF);
    }
  }

  IMB_buffer_float_from_byte(
      dst.data(), src.data(), width, height, dest_stride, src_stride);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float expected[4];
      rgba_uchar_to_float(expected, &src[(size_t(src_stride) * y + x) * 4]);
      const float *got = &dst[(size_t(dest_stride) * y + x) * 4];
      for (int c = 0; c < 4; c++) {
        ASSERT_EQ(got[c], expected[c]) << "y " << y << " x " << x << " channel " << c;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IMB_buffer_byte_from_float (float -> byte, no dither / no predivide)
 * \{ */

TEST(NeonPixelConversionTest, FloatToByte_DenseSweep)
{
  constexpr int width = 4001; /* Deliberately not a multiple of 4. */
  std::vector<float> src(size_t(width) * 4);
  for (int i = 0; i < width; i++) {
    /* Sweep [-0.5, 1.5] across the row, replicated into all 4 channels. */
    const float v = -0.5f + 2.0f * float(i) / float(width - 1);
    src[i * 4 + 0] = v;
    src[i * 4 + 1] = v;
    src[i * 4 + 2] = v;
    src[i * 4 + 3] = v;
  }
  std::vector<uchar> dst(size_t(width) * 4, 0xAA);

  IMB_buffer_byte_from_float(
      dst.data(), src.data(), 4, 0.0f, false, width, 1, width, 0);

  for (int i = 0; i < width; i++) {
    uchar expected[4];
    rgba_float_to_uchar(expected, &src[i * 4]);
    for (int c = 0; c < 4; c++) {
      ASSERT_EQ(dst[i * 4 + c], expected[c]) << "pixel " << i << " channel " << c;
    }
  }
}

TEST(NeonPixelConversionTest, FloatToByte_ExactBoundaryValues)
{
  const float thresh = 1.0f - 0.5f / 255.0f;
  const std::vector<float> boundary_values = {
      0.0f,
      -0.0f,
      1.0f,
      -1.0f,
      0.5f / 255.0f,
      std::nextafterf(0.5f / 255.0f, 1.0f),
      std::nextafterf(0.5f / 255.0f, 0.0f),
      thresh,
      std::nextafterf(thresh, 1.0f),
      std::nextafterf(thresh, 0.0f),
      std::nextafterf(0.0f, 1.0f),
      std::nextafterf(0.0f, -1.0f),
      std::numeric_limits<float>::denorm_min(),
      -std::numeric_limits<float>::denorm_min(),
      std::numeric_limits<float>::min(),
      1.5f,
      2.0f,
      100.0f,
  };
  const int width = int(boundary_values.size());
  std::vector<float> src(size_t(width) * 4);
  for (int i = 0; i < width; i++) {
    src[i * 4 + 0] = boundary_values[i];
    src[i * 4 + 1] = boundary_values[i];
    src[i * 4 + 2] = boundary_values[i];
    src[i * 4 + 3] = boundary_values[i];
  }
  std::vector<uchar> dst(size_t(width) * 4, 0xAA);

  IMB_buffer_byte_from_float(
      dst.data(), src.data(), 4, 0.0f, false, width, 1, width, 0);

  for (int i = 0; i < width; i++) {
    uchar expected[4];
    rgba_float_to_uchar(expected, &src[i * 4]);
    for (int c = 0; c < 4; c++) {
      EXPECT_EQ(dst[i * 4 + c], expected[c])
          << "value " << boundary_values[i] << " channel " << c;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IMB_premultiply_rect (byte)
 * \{ */

TEST(NeonPixelConversionTest, PremultiplyByte_ExhaustiveAllPairs)
{
  /* All 256*256 = 65536 (color, alpha) pairs, laid out as a 256x256 image (256 is a
   * multiple of the 16-pixel NEON batch, so every row is fully NEON-covered on an
   * __ARM_NEON build; the remainder-width test below covers the tail path). */
  constexpr int w = 256;
  constexpr int h = 256;
  std::vector<uchar> rect(size_t(w) * h * 4);
  for (int a = 0; a < h; a++) {
    for (int c = 0; c < w; c++) {
      uchar *px = &rect[(size_t(a) * w + c) * 4];
      px[0] = uchar(c);
      px[1] = uchar(255 - c);
      px[2] = uchar((c ^ 0x5A) & 0xFF);
      px[3] = uchar(a);
    }
  }
  std::vector<uchar> expected = rect;

  IMB_premultiply_rect(rect.data(), ImColorMode::RGBA, w, h);

  for (int a = 0; a < h; a++) {
    for (int c = 0; c < w; c++) {
      const size_t idx = (size_t(a) * w + c) * 4;
      const int val = expected[idx + 3];
      const uchar ref0 = uchar((int(expected[idx + 0]) * val) >> 8);
      const uchar ref1 = uchar((int(expected[idx + 1]) * val) >> 8);
      const uchar ref2 = uchar((int(expected[idx + 2]) * val) >> 8);
      ASSERT_EQ(rect[idx + 0], ref0) << "c=" << c << " a=" << a;
      ASSERT_EQ(rect[idx + 1], ref1) << "c=" << c << " a=" << a;
      ASSERT_EQ(rect[idx + 2], ref2) << "c=" << c << " a=" << a;
      ASSERT_EQ(rect[idx + 3], uchar(a)) << "alpha must be left unmodified";
    }
  }
}

TEST(NeonPixelConversionTest, PremultiplyByte_RemainderTailWidths)
{
  for (int w : {1, 3, 15, 17, 31, 33, 100}) {
    constexpr int h = 3;
    std::vector<uchar> rect(size_t(w) * h * 4);
    for (int i = 0; i < w * h; i++) {
      rect[i * 4 + 0] = uchar((i * 41 + 3) & 0xFF);
      rect[i * 4 + 1] = uchar((i * 53 + 7) & 0xFF);
      rect[i * 4 + 2] = uchar((i * 61 + 13) & 0xFF);
      rect[i * 4 + 3] = uchar((i * 29 + 17) & 0xFF);
    }
    std::vector<uchar> expected = rect;

    IMB_premultiply_rect(rect.data(), ImColorMode::RGBA, w, h);

    for (int i = 0; i < w * h; i++) {
      const int val = expected[i * 4 + 3];
      const uchar ref0 = uchar((int(expected[i * 4 + 0]) * val) >> 8);
      const uchar ref1 = uchar((int(expected[i * 4 + 1]) * val) >> 8);
      const uchar ref2 = uchar((int(expected[i * 4 + 2]) * val) >> 8);
      ASSERT_EQ(rect[i * 4 + 0], ref0) << "w=" << w << " i=" << i;
      ASSERT_EQ(rect[i * 4 + 1], ref1) << "w=" << w << " i=" << i;
      ASSERT_EQ(rect[i * 4 + 2], ref2) << "w=" << w << " i=" << i;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IMB_premultiply_rect_float / IMB_unpremultiply_rect_float
 * \{ */

TEST(NeonPixelConversionTest, PremultiplyFloat_Sweep)
{
  constexpr int width = 4001;
  std::vector<float> rect(size_t(width) * 4);
  for (int i = 0; i < width; i++) {
    const float t = float(i) / float(width - 1);
    rect[i * 4 + 0] = -1.0f + 3.0f * t;
    rect[i * 4 + 1] = 2.0f - 4.0f * t;
    rect[i * 4 + 2] = t * t;
    rect[i * 4 + 3] = -0.5f + 2.5f * t;
  }
  std::vector<float> expected = rect;

  IMB_premultiply_rect_float(rect.data(), 4, width, 1);

  for (int i = 0; i < width; i++) {
    const float val = expected[i * 4 + 3];
    EXPECT_EQ(rect[i * 4 + 0], expected[i * 4 + 0] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 1], expected[i * 4 + 1] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 2], expected[i * 4 + 2] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 3], expected[i * 4 + 3]) << "alpha must be left unmodified";
  }
}

TEST(NeonPixelConversionTest, UnpremultiplyFloat_SweepAndAlphaZeroRows)
{
  constexpr int width = 4001;
  std::vector<float> rect(size_t(width) * 4);
  for (int i = 0; i < width; i++) {
    const float t = float(i) / float(width - 1);
    rect[i * 4 + 0] = -1.0f + 3.0f * t;
    rect[i * 4 + 1] = 2.0f - 4.0f * t;
    rect[i * 4 + 2] = t * t;
    /* Every 7th pixel has alpha exactly 0.0f (or -0.0f) to exercise the guard branch
     * inside NEON batches, not just in isolation. */
    if (i % 14 == 0) {
      rect[i * 4 + 3] = 0.0f;
    }
    else if (i % 14 == 7) {
      rect[i * 4 + 3] = -0.0f;
    }
    else {
      rect[i * 4 + 3] = -0.5f + 2.5f * t;
    }
  }
  std::vector<float> expected = rect;

  IMB_unpremultiply_rect_float(rect.data(), 4, width, 1);

  for (int i = 0; i < width; i++) {
    const float alpha = expected[i * 4 + 3];
    const float val = alpha != 0.0f ? 1.0f / alpha : 1.0f;
    EXPECT_EQ(rect[i * 4 + 0], expected[i * 4 + 0] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 1], expected[i * 4 + 1] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 2], expected[i * 4 + 2] * val) << "i=" << i;
    EXPECT_EQ(rect[i * 4 + 3], expected[i * 4 + 3]) << "alpha must be left unmodified";
  }
}

TEST(NeonPixelConversionTest, UnpremultiplyFloat_AllAlphaZeroBatch)
{
  /* A full NEON batch (and its tail) where every alpha is exactly zero, so every lane
   * takes the guard branch simultaneously. */
  for (int width : {4, 5, 16, 17}) {
    std::vector<float> rect(size_t(width) * 4);
    for (int i = 0; i < width; i++) {
      rect[i * 4 + 0] = 1.0f + float(i);
      rect[i * 4 + 1] = -2.0f - float(i);
      rect[i * 4 + 2] = 0.0f;
      rect[i * 4 + 3] = 0.0f;
    }
    std::vector<float> expected = rect;

    IMB_unpremultiply_rect_float(rect.data(), 4, width, 1);

    for (int i = 0; i < width; i++) {
      /* val = 1.0f since alpha == 0, so result == original color unchanged. */
      EXPECT_EQ(rect[i * 4 + 0], expected[i * 4 + 0]) << "width=" << width << " i=" << i;
      EXPECT_EQ(rect[i * 4 + 1], expected[i * 4 + 1]) << "width=" << width << " i=" << i;
      EXPECT_EQ(rect[i * 4 + 2], expected[i * 4 + 2]) << "width=" << width << " i=" << i;
    }
  }
}

/** \} */

}  // namespace blender::imbuf::tests
