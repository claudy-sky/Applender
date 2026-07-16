/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "BLI_array.hh"
#include "BLI_rect.hh"
#include "BLI_task.hh"

#include "IMB_filter.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "IMB_colormanagement.hh"

#include "MEM_guardedalloc.h"

#include "OCIO_colorspace.hh"

#if defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

/* Bit-exact NEON<->scalar parity in this file depends on multiply+add pairs
 * NOT contracting into FMA on either side: the scalar reference
 * (255.0f * val) + 0.5f rounds twice, and the NEON path deliberately uses
 * separate vmulq/vaddq. The build sets -ffp-contract=off globally; pin it
 * per-TU so the parity cannot silently break if that flag ever changes. */
#if defined(__clang__)
#  pragma clang fp contract(off)
#endif

namespace blender {

#if defined(__ARM_NEON)
/* -------------------------------------------------------------------- */
/** \name NEON byte <-> float pixel conversion (Apple Silicon / Metal-only build)
 *
 * These are bit-exact replacements for the scalar per-pixel loops used by
 * #IMB_buffer_float_from_byte and the no-dither/no-predivide branch of
 * #IMB_buffer_byte_from_float. Reference semantics (verified in
 * math_color_inline.cc / math_base_inline.cc):
 *
 *   rgba_uchar_to_float(): r_col[i] = float(col_ub[i]) * (1.0f / 255.0f)
 *
 *   rgba_float_to_uchar() -> unit_float_to_uchar_clamp_v4() -> per component:
 *     unit_float_to_uchar_clamp(val) =
 *       (val <= 0.0f)                    ? 0   :
 *       (val > (1.0f - 0.5f / 255.0f))   ? 255 :
 *                                          uchar(255.0f * val + 0.5f)  // truncating cast
 *
 *   NaN is UB in the scalar reference (both comparisons are false, so the final
 *   `static_cast<unsigned char>(NaN)` is executed, which is undefined behavior).
 *   The NEON helpers below are therefore only required to match on non-NaN input,
 *   and callers/tests must not probe NaN.
 * \{ */

/* Convert 4 interleaved RGBA8 pixels (16 bytes) at `from` to 4 interleaved RGBA float
 * pixels (16 floats) at `to`. Bit-exact with 4x `rgba_uchar_to_float()`. */
static inline void rgba_uchar_to_float_neon4(float *to, const uchar *from)
{
  const uint8x16_t bytes = vld1q_u8(from);
  const uint16x8_t lo16 = vmovl_u8(vget_low_u8(bytes));
  const uint16x8_t hi16 = vmovl_u8(vget_high_u8(bytes));
  const uint32x4_t v0 = vmovl_u16(vget_low_u16(lo16));
  const uint32x4_t v1 = vmovl_u16(vget_high_u16(lo16));
  const uint32x4_t v2 = vmovl_u16(vget_low_u16(hi16));
  const uint32x4_t v3 = vmovl_u16(vget_high_u16(hi16));
  /* Single multiply per lane by the same rounded (1.0f/255.0f) constant the scalar
   * path uses -- matches `float(c) * (1.0f / 255.0f)` bit-for-bit (multiplication is
   * commutative and exactly reproducible in IEEE-754, so operand order does not matter). */
  const float32x4_t scale = vdupq_n_f32(1.0f / 255.0f);
  vst1q_f32(to + 0, vmulq_f32(vcvtq_f32_u32(v0), scale));
  vst1q_f32(to + 4, vmulq_f32(vcvtq_f32_u32(v1), scale));
  vst1q_f32(to + 8, vmulq_f32(vcvtq_f32_u32(v2), scale));
  vst1q_f32(to + 12, vmulq_f32(vcvtq_f32_u32(v3), scale));
}

/* Clamp/convert one float32x4 lane group to bytes, reproducing
 * `unit_float_to_uchar_clamp()` exactly:
 *  - `vmulq_f32` + `vaddq_f32` (NOT a fused multiply-add) to match the two separately
 *    rounded scalar operations `(255.0f * val) + 0.5f`.
 *  - `vcvtq_u32_f32` truncates toward zero (FCVTZU), matching the scalar's truncating
 *    `static_cast<unsigned char>` on an always-non-negative value.
 *  - Lanes that are out-of-range for the "else" branch (val <= 0, or val absurdly large)
 *    may compute garbage in `trunc`, but they are always overridden by the `is_le0` /
 *    `is_gt_thresh` select below, so the garbage is never observed. */
static inline uint32x4_t clamp_float_to_byte_u32(float32x4_t v)
{
  const uint32x4_t is_le0 = vcleq_f32(v, vdupq_n_f32(0.0f));
  const uint32x4_t is_gt_thresh = vcgtq_f32(v, vdupq_n_f32(1.0f - 0.5f / 255.0f));
  const float32x4_t scaled = vaddq_f32(vmulq_f32(v, vdupq_n_f32(255.0f)), vdupq_n_f32(0.5f));
  const uint32x4_t trunc = vcvtq_u32_f32(scaled);
  const uint32x4_t clamped_lo = vbslq_u32(is_le0, vdupq_n_u32(0), trunc);
  return vbslq_u32(is_gt_thresh, vdupq_n_u32(255), clamped_lo);
}

/* Convert 4 interleaved RGBA float pixels (16 floats) at `from` to 4 interleaved
 * RGBA8 pixels (16 bytes) at `to`. Bit-exact with 4x `rgba_float_to_uchar()`. */
static inline void rgba_float_to_uchar_neon4(uchar *to, const float *from)
{
  const uint32x4_t v0 = clamp_float_to_byte_u32(vld1q_f32(from + 0));
  const uint32x4_t v1 = clamp_float_to_byte_u32(vld1q_f32(from + 4));
  const uint32x4_t v2 = clamp_float_to_byte_u32(vld1q_f32(from + 8));
  const uint32x4_t v3 = clamp_float_to_byte_u32(vld1q_f32(from + 12));
  const uint8x8_t b0 = vmovn_u16(vcombine_u16(vmovn_u32(v0), vmovn_u32(v1)));
  const uint8x8_t b1 = vmovn_u16(vcombine_u16(vmovn_u32(v2), vmovn_u32(v3)));
  vst1q_u8(to, vcombine_u8(b0, b1));
}

/** \} */
#endif  /* __ARM_NEON */

/* -------------------------------------------------------------------- */

/** \name Generic Buffer Conversion
 * \{ */

MINLINE uchar ftochar(float value)
{
  return unit_float_to_uchar_clamp(value);
}

MINLINE void float_to_byte_dither_v4(uchar b[4], const float f[4], float dither, int x, int y)
{
  float dither_value = dither_random_value(x, y) * 0.0033f * dither;

  b[0] = ftochar(dither_value + f[0]);
  b[1] = ftochar(dither_value + f[1]);
  b[2] = ftochar(dither_value + f[2]);
  b[3] = unit_float_to_uchar_clamp(f[3]);
}

bool IMB_alpha_affects_rgb(const ImBuf *ibuf)
{
  return ibuf && !flag_is_set(ibuf->flags, ImBufFlags::AlphaChannelPacked);
}

void IMB_buffer_byte_from_float(uchar *dest,
                                const float *src,
                                int src_channels,
                                float dither,
                                bool predivide,
                                int width,
                                int height,
                                int stride,
                                int start_y)
{
  for (int y = 0; y < height; y++) {
    const float *from = src + size_t(stride) * y * src_channels;
    uchar *to = dest + size_t(stride) * y * 4;
    if (src_channels == 1) {
      /* single channel input */
      for (int x = 0; x < width; x++, from++, to += 4) {
        to[0] = to[1] = to[2] = to[3] = unit_float_to_uchar_clamp(from[0]);
      }
    }
    else if (src_channels == 3) {
      /* RGB input */
      for (int x = 0; x < width; x++, from += 3, to += 4) {
        rgb_float_to_uchar(to, from);
        to[3] = 255;
      }
    }
    else if (src_channels == 4) {
      /* RGBA input */
      if (dither && predivide) {
        float straight[4];
        for (int x = 0; x < width; x++, from += 4, to += 4) {
          premul_to_straight_v4_v4(straight, from);
          float_to_byte_dither_v4(to, straight, dither, x, y + start_y);
        }
      }
      else if (dither) {
        for (int x = 0; x < width; x++, from += 4, to += 4) {
          float_to_byte_dither_v4(to, from, dither, x, y + start_y);
        }
      }
      else if (predivide) {
        for (int x = 0; x < width; x++, from += 4, to += 4) {
          premul_float_to_straight_uchar(to, from);
        }
      }
      else {
        int x = 0;
#if defined(__ARM_NEON)
        for (; x + 4 <= width; x += 4, from += 16, to += 16) {
          rgba_float_to_uchar_neon4(to, from);
        }
#endif
        for (; x < width; x++, from += 4, to += 4) {
          rgba_float_to_uchar(to, from);
        }
      }
    }
  }
}

void IMB_buffer_byte_from_float_mask(uchar *dest,
                                     const float *src,
                                     int src_channels,
                                     float dither,
                                     int width,
                                     int height,
                                     const char *mask)
{
  for (int y = 0; y < height; y++) {
    const float *from = src + size_t(width) * y * src_channels;
    uchar *to = dest + size_t(width) * y * 4;
    if (src_channels == 1) {
      /* single channel input */
      for (int x = 0; x < width; x++, from++, to += 4) {
        if (*mask++ == FILTER_MASK_USED) {
          to[0] = to[1] = to[2] = to[3] = unit_float_to_uchar_clamp(from[0]);
        }
      }
    }
    else if (src_channels == 3) {
      /* RGB input */
      for (int x = 0; x < width; x++, from += 3, to += 4) {
        if (*mask++ == FILTER_MASK_USED) {
          rgb_float_to_uchar(to, from);
          to[3] = 255;
        }
      }
    }
    else if (src_channels == 4) {
      /* RGBA input */
      if (dither) {
        for (int x = 0; x < width; x++, from += 4, to += 4) {
          if (*mask++ == FILTER_MASK_USED) {
            float_to_byte_dither_v4(to, from, dither, x, y);
          }
        }
      }
      else {
        for (int x = 0; x < width; x++, from += 4, to += 4) {
          if (*mask++ == FILTER_MASK_USED) {
            rgba_float_to_uchar(to, from);
          }
        }
      }
    }
  }
}

void IMB_buffer_float_from_byte(
    float *dest, const uchar *src, int width, int height, int dest_stride, int src_stride)
{
  for (int y = 0; y < height; y++) {
    const uchar *from = src + size_t(src_stride) * y * 4;
    float *to = dest + size_t(dest_stride) * y * 4;

    int x = 0;
#if defined(__ARM_NEON)
    for (; x + 4 <= width; x += 4, from += 16, to += 16) {
      rgba_uchar_to_float_neon4(to, from);
    }
#endif
    for (; x < width; x++, from += 4, to += 4) {
      rgba_uchar_to_float(to, from);
    }
  }
}

void IMB_buffer_float_rgba_from_float(
    float *dest, const float *src, int src_channels, int width, int height)
{
  if (src_channels == 1) {
    /* single channel input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y;
      float *to = dest + size_t(width) * y * 4;

      for (int x = 0; x < width; x++, from++, to += 4) {
        to[0] = to[1] = to[2] = to[3] = from[0];
      }
    }
  }
  else if (src_channels == 3) {
    /* RGB input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y * 3;
      float *to = dest + size_t(width) * y * 4;

      for (int x = 0; x < width; x++, from += 3, to += 4) {
        copy_v3_v3(to, from);
        to[3] = 1.0f;
      }
    }
  }
  else if (src_channels == 4) {
    /* RGBA input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y * 4;
      float *to = dest + size_t(width) * y * 4;
      memcpy(to, from, sizeof(float) * size_t(4) * width);
    }
  }
}

void IMB_buffer_float_rgba_from_float_mask(
    float *dest, const float *src, int src_channels, int width, int height, const char *mask)
{
  if (src_channels == 1) {
    /* single channel input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y;
      float *to = dest + size_t(width) * y * 4;

      for (int x = 0; x < width; x++, from++, to += 4) {
        if (*mask++ == FILTER_MASK_USED) {
          to[0] = to[1] = to[2] = to[3] = from[0];
        }
      }
    }
  }
  else if (src_channels == 3) {
    /* RGB input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y * 3;
      float *to = dest + size_t(width) * y * 4;

      for (int x = 0; x < width; x++, from += 3, to += 4) {
        if (*mask++ == FILTER_MASK_USED) {
          copy_v3_v3(to, from);
          to[3] = 1.0f;
        }
      }
    }
  }
  else if (src_channels == 4) {
    /* RGBA input */
    for (int y = 0; y < height; y++) {
      const float *from = src + size_t(width) * y * 4;
      float *to = dest + size_t(width) * y * 4;

      for (int x = 0; x < width; x++, from += 4, to += 4) {
        if (*mask++ == FILTER_MASK_USED) {
          copy_v4_v4(to, from);
        }
      }
    }
  }
}

void IMB_buffer_float_rgba_srgb_to_linear(float *buffer, int width, int height)
{
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    for (int y : y_range) {
      float *ptr = buffer + size_t(width) * y * 4;
      for (int x = 0; x < width; x++, ptr += 4) {
        srgb_to_linearrgb_predivide_v4(ptr, ptr);
      }
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ImBuf Conversion
 * \{ */

void IMB_byte_from_float(ImBuf *ibuf)
{
  /* Nothing to do if there's no float buffer */
  const float *float_data = ibuf->float_data();
  if (float_data == nullptr) {
    return;
  }

  /* Allocate byte buffer if needed. */
  if (ibuf->byte_data() == nullptr) {
    if (!IMB_alloc_byte_pixels(ibuf, false)) {
      return;
    }
  }

  const char *from_colorspace = IMB_colormanagement_get_float_colorspace(ibuf);
  const char *to_colorspace = IMB_colormanagement_get_byte_colorspace(ibuf);
  const bool predivide = IMB_alpha_affects_rgb(ibuf);
  ColormanageProcessor processor = ColormanageProcessor::colorspace_processor_new(from_colorspace,
                                                                                  to_colorspace);

  /* At 4 floats per pixel, this is 32KB of data, and fits into typical CPU L1 cache. */
  static constexpr int grain_size = 2048;
  uchar *byte_data = ibuf->byte_data_for_write();
  threading::parallel_for(
      IndexRange(IMB_get_pixel_count(ibuf)), grain_size, [&](const IndexRange range) {
        /* Copy chunk of source float pixels into a local buffer. */
        Array<float, grain_size * 4> buffer(range.size() * ibuf->channels);
        buffer.as_mutable_span().copy_from(
            Span(float_data + range.first() * ibuf->channels, buffer.size()));
        /* Unpremultiply alpha if needed. */
        if (predivide) {
          IMB_unpremultiply_rect_float(buffer.data(), ibuf->channels, range.size(), 1);
        }
        /* Convert to byte color space if needed. */
        if (!processor.is_noop()) {
          processor.apply(buffer.data(), range.size(), 1, ibuf->channels, false);
        }
        /* Convert to bytes. */
        IMB_buffer_byte_from_float(byte_data + range.first() * 4,
                                   buffer.data(),
                                   ibuf->channels,
                                   ibuf->dither,
                                   false,
                                   range.size(),
                                   1,
                                   ibuf->x);
      });
}

void IMB_float_from_byte_ex(ImBuf *dst, const ImBuf *src, const rcti *region_to_update)
{
  BLI_assert_msg(dst->float_data() != nullptr,
                 "Destination buffer should have a float buffer assigned.");
  BLI_assert_msg(src->byte_data() != nullptr, "Source buffer should have a byte buffer assigned.");
  BLI_assert_msg(dst->x == src->x, "Source and destination buffer should have the same dimension");
  BLI_assert_msg(dst->y == src->y, "Source and destination buffer should have the same dimension");
  BLI_assert_msg(dst->channels = 4, "Destination buffer should have 4 channels.");
  BLI_assert_msg(region_to_update->xmin >= 0,
                 "Region to update should be clipped to the given buffers.");
  BLI_assert_msg(region_to_update->ymin >= 0,
                 "Region to update should be clipped to the given buffers.");
  BLI_assert_msg(region_to_update->xmax <= dst->x,
                 "Region to update should be clipped to the given buffers.");
  BLI_assert_msg(region_to_update->ymax <= dst->y,
                 "Region to update should be clipped to the given buffers.");

  const int region_width = BLI_rcti_size_x(region_to_update);
  const int region_height = BLI_rcti_size_y(region_to_update);
  const bool premultiply_alpha = IMB_alpha_affects_rgb(src);

  const uchar *byte_data = src->byte_data();
  float *float_data = dst->float_data_for_write();
  threading::parallel_for(
      IndexRange(region_to_update->ymin, region_height), 64, [&](const IndexRange y_range) {
        const uchar *src_ptr = byte_data + (region_to_update->xmin + y_range.first() * dst->x) * 4;
        float *dst_ptr = float_data + (region_to_update->xmin + y_range.first() * dst->x) * 4;

        /* Convert byte -> float without color or alpha conversions. */
        IMB_buffer_float_from_byte(dst_ptr, src_ptr, region_width, y_range.size(), src->x, dst->x);

        /* Convert to scene linear color space, and premultiply alpha if needed. */
        float *dst_ptr_line = dst_ptr;
        for ([[maybe_unused]] const int64_t y : y_range) {
          IMB_colormanagement_colorspace_to_scene_linear(
              dst_ptr_line, region_width, 1, dst->channels, src->byte_buffer.colorspace, false);
          if (premultiply_alpha) {
            IMB_premultiply_rect_float(dst_ptr_line, dst->channels, region_width, 1);
          }
          dst_ptr_line += 4 * dst->x;
        }
      });
}

void IMB_float_from_byte(ImBuf *ibuf)
{
  /* Nothing to do if there's no byte buffer. */
  if (ibuf->byte_data() == nullptr) {
    return;
  }

  /* Allocate float buffer if needed. */
  if (ibuf->float_data() == nullptr) {
    if (!IMB_alloc_float_pixels(ibuf, 4, false)) {
      return;
    }
  }

  rcti region_to_update;
  BLI_rcti_init(&region_to_update, 0, ibuf->x, 0, ibuf->y);
  IMB_float_from_byte_ex(ibuf, ibuf, &region_to_update);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color to Gray-Scale
 * \{ */

void IMB_color_to_bw(ImBuf *ibuf)
{
  float *rct_fl = ibuf->float_data_for_write();
  uchar *rct = ibuf->byte_data_for_write();
  size_t i;

  if (rct_fl) {
    if (ibuf->channels >= 3) {
      for (i = IMB_get_pixel_count(ibuf); i > 0; i--, rct_fl += ibuf->channels) {
        rct_fl[0] = rct_fl[1] = rct_fl[2] = IMB_colormanagement_get_luminance(rct_fl);
      }
    }
  }

  if (rct) {
    for (i = IMB_get_pixel_count(ibuf); i > 0; i--, rct += 4) {
      rct[0] = rct[1] = rct[2] = IMB_colormanagement_get_luminance_byte(rct);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alter Saturation
 * \{ */

void IMB_saturation(ImBuf *ibuf, float sat)
{
  const size_t pixel_count = IMB_get_pixel_count(ibuf);
  if (uchar *byte_data = ibuf->byte_data_for_write()) {
    threading::parallel_for(IndexRange(pixel_count), 64 * 1024, [&](IndexRange range) {
      uchar *ptr = byte_data + range.first() * 4;
      float rgb[3];
      float hsv[3];
      for ([[maybe_unused]] const int64_t i : range) {
        rgb_uchar_to_float(rgb, ptr);
        rgb_to_hsv_v(rgb, hsv);
        hsv_to_rgb(hsv[0], hsv[1] * sat, hsv[2], rgb + 0, rgb + 1, rgb + 2);
        rgb_float_to_uchar(ptr, rgb);
        ptr += 4;
      }
    });
  }

  float *float_data = ibuf->float_data_for_write();
  if (float_data != nullptr && ibuf->channels >= 3) {
    threading::parallel_for(IndexRange(pixel_count), 64 * 1024, [&](IndexRange range) {
      const int channels = ibuf->channels;
      float *ptr = float_data + range.first() * channels;
      float hsv[3];
      for ([[maybe_unused]] const int64_t i : range) {
        rgb_to_hsv_v(ptr, hsv);
        hsv_to_rgb(hsv[0], hsv[1] * sat, hsv[2], ptr + 0, ptr + 1, ptr + 2);
        ptr += channels;
      }
    });
  }
}

/** \} */

}  // namespace blender
