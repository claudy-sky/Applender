/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_base_c.hh"
#include "BLI_utildefines.hh"

#include "IMB_filter.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#if defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

/* See conversion.cc: NEON<->scalar parity requires no FMA contraction. */
#if defined(__clang__)
#  pragma clang fp contract(off)
#endif

namespace blender {

#if defined(__ARM_NEON)
/* -------------------------------------------------------------------- */
/** \name NEON premultiply / unpremultiply (Apple Silicon / Metal-only build)
 * \{ */

/* Premultiply 16 interleaved RGBA8 pixels (64 bytes) in place at `cp`.
 * Bit-exact with `cp[c] = (cp[c] * cp[3]) >> 8` for c in {0,1,2}; alpha (cp[3])
 * is left unmodified, matching the scalar loop.
 * `vmull_u8` widens to u16 before the multiply, and since 255*255 = 65025 < 65536
 * the product never overflows u16, so the `>> 8` (vshrq_n_u16) truncates exactly
 * like the scalar's `int` right-shift on a non-negative value. */
static inline void premultiply_rect_neon16(uint8_t *cp)
{
  uint8x16x4_t px = vld4q_u8(cp);
  const uint8x16_t a = px.val[3];
  const uint8x8_t a_lo = vget_low_u8(a);
  const uint8x8_t a_hi = vget_high_u8(a);
  for (int c = 0; c < 3; c++) {
    const uint8x8_t ch_lo = vget_low_u8(px.val[c]);
    const uint8x8_t ch_hi = vget_high_u8(px.val[c]);
    const uint16x8_t prod_lo = vshrq_n_u16(vmull_u8(ch_lo, a_lo), 8);
    const uint16x8_t prod_hi = vshrq_n_u16(vmull_u8(ch_hi, a_hi), 8);
    px.val[c] = vcombine_u8(vmovn_u16(prod_lo), vmovn_u16(prod_hi));
  }
  vst4q_u8(cp, px);
}

/* Premultiply 4 interleaved RGBA float pixels (16 floats) in place at `cp`.
 * Bit-exact with `cp[c] = cp[c] * cp[3]` for c in {0,1,2} (plain `vmulq_f32`, no FMA,
 * matching the single scalar multiply exactly -- multiplication is commutative and
 * exactly reproducible in IEEE-754, so lane order/operand order does not matter). */
static inline void premultiply_rect_float_neon4(float *cp)
{
  float32x4x4_t px = vld4q_f32(cp);
  const float32x4_t a = px.val[3];
  px.val[0] = vmulq_f32(px.val[0], a);
  px.val[1] = vmulq_f32(px.val[1], a);
  px.val[2] = vmulq_f32(px.val[2], a);
  vst4q_f32(cp, px);
}

/* Unpremultiply 4 interleaved RGBA float pixels (16 floats) in place at `cp`.
 * Bit-exact with:
 *   val = (alpha != 0.0f) ? 1.0f / alpha : 1.0f;
 *   cp[c] = cp[c] * val;  // for c in {0,1,2}
 * Uses exact division (`vdivq_f32`), never a reciprocal estimate, so `1.0f / alpha`
 * matches the scalar division bit-for-bit. Lanes with alpha == 0 compute
 * `1.0f / 0.0f` (well-defined IEEE-754 +-infinity, no trap), but that transient
 * value is always replaced by 1.0f via the select before the final multiply, so it
 * is never observed in the result. */
static inline void unpremultiply_rect_float_neon4(float *cp)
{
  float32x4x4_t px = vld4q_f32(cp);
  const float32x4_t a = px.val[3];
  const uint32x4_t is_zero = vceqq_f32(a, vdupq_n_f32(0.0f));
  const float32x4_t inv = vdivq_f32(vdupq_n_f32(1.0f), a);
  const float32x4_t val = vbslq_f32(is_zero, vdupq_n_f32(1.0f), inv);
  px.val[0] = vmulq_f32(px.val[0], val);
  px.val[1] = vmulq_f32(px.val[1], val);
  px.val[2] = vmulq_f32(px.val[2], val);
  vst4q_f32(cp, px);
}

/** \} */
#endif  /* __ARM_NEON */

static void filtcolum(uchar *point, int y, int skip)
{
  uint c1, c2, c3, error;
  uchar *point2;

  if (y > 1) {
    c1 = c2 = *point;
    point2 = point;
    error = 2;
    for (y--; y > 0; y--) {
      point2 += skip;
      c3 = *point2;
      c1 += (c2 << 1) + c3 + error;
      error = c1 & 3;
      *point = c1 >> 2;
      point = point2;
      c1 = c2;
      c2 = c3;
    }
    *point = (c1 + (c2 << 1) + c2 + error) >> 2;
  }
}

static void filtcolumf(float *point, int y, int skip)
{
  float c1, c2, c3, *point2;

  if (y > 1) {
    c1 = c2 = *point;
    point2 = point;
    for (y--; y > 0; y--) {
      point2 += skip;
      c3 = *point2;
      c1 += (c2 * 2) + c3;
      *point = 0.25f * c1;
      point = point2;
      c1 = c2;
      c2 = c3;
    }
    *point = 0.25f * (c1 + (c2 * 2) + c2);
  }
}

void IMB_filtery(ImBuf *ibuf)
{
  uchar *point = ibuf->byte_data_for_write();
  float *pointf = ibuf->float_data_for_write();

  int x = ibuf->x;
  int y = ibuf->y;
  int skip = x << 2;

  for (; x > 0; x--) {
    if (point) {
      if (ibuf->color_mode == ImColorMode::RGBA) {
        filtcolum(point, y, skip);
      }
      point++;
      filtcolum(point, y, skip);
      point++;
      filtcolum(point, y, skip);
      point++;
      filtcolum(point, y, skip);
      point++;
    }
    if (pointf) {
      if (ibuf->color_mode == ImColorMode::RGBA) {
        filtcolumf(pointf, y, skip);
      }
      pointf++;
      filtcolumf(pointf, y, skip);
      pointf++;
      filtcolumf(pointf, y, skip);
      pointf++;
      filtcolumf(pointf, y, skip);
      pointf++;
    }
  }
}

void IMB_mask_filter_extend(char *mask, int width, int height)
{
  const char *row1, *row2, *row3;
  int rowlen, x, y;
  char *temprect;

  rowlen = width;

  /* make a copy, to prevent flooding */
  temprect = MEM_dupalloc(mask);

  for (y = 1; y <= height; y++) {
    /* setup rows */
    row1 = static_cast<char *>(temprect + (y - 2) * rowlen);
    row2 = row1 + rowlen;
    row3 = row2 + rowlen;
    if (y == 1) {
      row1 = row2;
    }
    else if (y == height) {
      row3 = row2;
    }

    for (x = 0; x < rowlen; x++) {
      if (mask[((y - 1) * rowlen) + x] == 0) {
        if (*row1 || *row2 || *row3 || *(row1 + 1) || *(row3 + 1)) {
          mask[((y - 1) * rowlen) + x] = FILTER_MASK_MARGIN;
        }
        else if ((x != rowlen - 1) && (*(row1 + 2) || *(row2 + 2) || *(row3 + 2))) {
          mask[((y - 1) * rowlen) + x] = FILTER_MASK_MARGIN;
        }
      }

      if (x != 0) {
        row1++;
        row2++;
        row3++;
      }
    }
  }

  MEM_delete(temprect);
}

void IMB_mask_clear(ImBuf *ibuf, const char *mask, int val)
{
  int x, y;
  if (float *float_data = ibuf->float_data_for_write()) {
    for (x = 0; x < ibuf->x; x++) {
      for (y = 0; y < ibuf->y; y++) {
        if (mask[ibuf->x * y + x] == val) {
          float *col = float_data + 4 * (ibuf->x * y + x);
          col[0] = col[1] = col[2] = col[3] = 0.0f;
        }
      }
    }
  }
  else {
    uchar *byte_data = ibuf->byte_data_for_write();
    /* char buffer */
    for (x = 0; x < ibuf->x; x++) {
      for (y = 0; y < ibuf->y; y++) {
        if (mask[ibuf->x * y + x] == val) {
          char *col = reinterpret_cast<char *>(byte_data + 4 * ibuf->x * y + x);
          col[0] = col[1] = col[2] = col[3] = 0;
        }
      }
    }
  }
}

static int filter_make_index(const int x, const int y, const int w, const int h)
{
  if (x < 0 || x >= w || y < 0 || y >= h) {
    return -1; /* return bad index */
  }

  return y * w + x;
}

static int check_pixel_assigned(
    const void *buffer, const char *mask, const int index, const int depth, const bool is_float)
{
  int res = 0;

  if (index >= 0) {
    const int alpha_index = depth * index + (depth - 1);

    if (mask != nullptr) {
      res = mask[index] != 0 ? 1 : 0;
    }
    else if ((is_float && (static_cast<const float *>(buffer))[alpha_index] != 0.0f) ||
             (!is_float && (static_cast<const uchar *>(buffer))[alpha_index] != 0))
    {
      res = 1;
    }
  }

  return res;
}

void IMB_filter_extend(ImBuf *ibuf, char *mask, int filter)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int depth = 4; /* always 4 channels */
  const int chsize = ibuf->float_data() ? sizeof(float) : sizeof(uchar);
  const size_t bsize = size_t(width) * height * depth * chsize;
  const bool is_float = (ibuf->float_data() != nullptr);
  void *dstbuf = ibuf->float_data() ? static_cast<void *>(MEM_dupalloc(ibuf->float_data())) :
                                      static_cast<void *>(MEM_dupalloc(ibuf->byte_data()));
  char *dstmask = mask == nullptr ? nullptr : MEM_dupalloc(mask);
  void *srcbuf = ibuf->float_data() ? static_cast<void *>(ibuf->float_data_for_write()) :
                                      static_cast<void *>(ibuf->byte_data_for_write());
  char *srcmask = mask;
  int cannot_early_out = 1, r, n, k, i, j, c;
  float weight[25];

  /* build a weights buffer */
  n = 1;

#if 0
  k = 0;
  for (i = -n; i <= n; i++) {
    for (j = -n; j <= n; j++) {
      weight[k++] = sqrt(float(i) * i + j * j);
    }
  }
#endif

  weight[0] = 1;
  weight[1] = 2;
  weight[2] = 1;
  weight[3] = 2;
  weight[4] = 0;
  weight[5] = 2;
  weight[6] = 1;
  weight[7] = 2;
  weight[8] = 1;

  /* run passes */
  for (r = 0; cannot_early_out == 1 && r < filter; r++) {
    int x, y;
    cannot_early_out = 0;

    for (y = 0; y < height; y++) {
      for (x = 0; x < width; x++) {
        const int index = filter_make_index(x, y, width, height);

        /* only update unassigned pixels */
        if (!check_pixel_assigned(srcbuf, srcmask, index, depth, is_float)) {
          float tmp[4];
          float wsum = 0;
          float acc[4] = {0, 0, 0, 0};
          k = 0;

          if (check_pixel_assigned(
                  srcbuf, srcmask, filter_make_index(x - 1, y, width, height), depth, is_float) ||
              check_pixel_assigned(
                  srcbuf, srcmask, filter_make_index(x + 1, y, width, height), depth, is_float) ||
              check_pixel_assigned(
                  srcbuf, srcmask, filter_make_index(x, y - 1, width, height), depth, is_float) ||
              check_pixel_assigned(
                  srcbuf, srcmask, filter_make_index(x, y + 1, width, height), depth, is_float))
          {
            for (i = -n; i <= n; i++) {
              for (j = -n; j <= n; j++) {
                if (i != 0 || j != 0) {
                  const int tmpindex = filter_make_index(x + i, y + j, width, height);

                  if (check_pixel_assigned(srcbuf, srcmask, tmpindex, depth, is_float)) {
                    if (is_float) {
                      for (c = 0; c < depth; c++) {
                        tmp[c] = (static_cast<const float *>(srcbuf))[depth * tmpindex + c];
                      }
                    }
                    else {
                      for (c = 0; c < depth; c++) {
                        tmp[c] = float((static_cast<const uchar *>(srcbuf))[depth * tmpindex + c]);
                      }
                    }

                    wsum += weight[k];

                    for (c = 0; c < depth; c++) {
                      acc[c] += weight[k] * tmp[c];
                    }
                  }
                }
                k++;
              }
            }

            if (wsum != 0) {
              for (c = 0; c < depth; c++) {
                acc[c] /= wsum;
              }

              if (is_float) {
                for (c = 0; c < depth; c++) {
                  (static_cast<float *>(dstbuf))[depth * index + c] = acc[c];
                }
              }
              else {
                for (c = 0; c < depth; c++) {
                  (static_cast<uchar *>(dstbuf))[depth * index + c] =
                      acc[c] > 255 ? 255 : (acc[c] < 0 ? 0 : uchar(roundf(acc[c])));
                }
              }

              if (dstmask != nullptr) {
                dstmask[index] = FILTER_MASK_MARGIN; /* assigned */
              }
              cannot_early_out = 1;
            }
          }
        }
      }
    }

    /* keep the original buffer up to date. */
    memcpy(srcbuf, dstbuf, bsize);
    if (dstmask != nullptr) {
      memcpy(srcmask, dstmask, size_t(width) * height);
    }
  }

  /* free memory */
  MEM_delete_void(dstbuf);
  if (dstmask != nullptr) {
    MEM_delete(dstmask);
  }
}

void IMB_premultiply_rect(uint8_t *rect, ImColorMode color_mode, int w, int h)
{
  uint8_t *cp;
  int x, y, val;

  if (color_mode == ImColorMode::RGB) { /* put alpha at 255 */
    cp = rect;

    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++, cp += 4) {
        cp[3] = 255;
      }
    }
  }
  else {
    cp = rect;

    for (y = 0; y < h; y++) {
      x = 0;
#if defined(__ARM_NEON)
      for (; x + 16 <= w; x += 16, cp += 64) {
        premultiply_rect_neon16(cp);
      }
#endif
      for (; x < w; x++, cp += 4) {
        val = cp[3];
        cp[0] = (cp[0] * val) >> 8;
        cp[1] = (cp[1] * val) >> 8;
        cp[2] = (cp[2] * val) >> 8;
      }
    }
  }
}

void IMB_premultiply_rect_float(float *rect_float, int channels, int w, int h)
{
  float val, *cp;
  int x, y;

  if (channels == 4) {
    cp = rect_float;
    for (y = 0; y < h; y++) {
      x = 0;
#if defined(__ARM_NEON)
      for (; x + 4 <= w; x += 4, cp += 16) {
        premultiply_rect_float_neon4(cp);
      }
#endif
      for (; x < w; x++, cp += 4) {
        val = cp[3];
        cp[0] = cp[0] * val;
        cp[1] = cp[1] * val;
        cp[2] = cp[2] * val;
      }
    }
  }
}

void IMB_premultiply_alpha(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  if (uchar *byte_data = ibuf->byte_data_for_write()) {
    IMB_premultiply_rect(byte_data, ibuf->color_mode, ibuf->x, ibuf->y);
  }

  if (float *float_data = ibuf->float_data_for_write()) {
    IMB_premultiply_rect_float(float_data, ibuf->channels, ibuf->x, ibuf->y);
  }
}

void IMB_unpremultiply_rect(uint8_t *rect, ImColorMode color_mode, int w, int h)
{
  uchar *cp;
  int x, y;
  float val;

  if (color_mode == ImColorMode::RGB) { /* put alpha at 255 */
    cp = rect;

    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++, cp += 4) {
        cp[3] = 255;
      }
    }
  }
  else {
    cp = rect;

    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++, cp += 4) {
        val = cp[3] != 0 ? 1.0f / float(cp[3]) : 1.0f;
        cp[0] = unit_float_to_uchar_clamp(cp[0] * val);
        cp[1] = unit_float_to_uchar_clamp(cp[1] * val);
        cp[2] = unit_float_to_uchar_clamp(cp[2] * val);
      }
    }
  }
}

void IMB_unpremultiply_rect_float(float *rect_float, int channels, int w, int h)
{
  float val, *fp;
  int x, y;

  if (channels == 4) {
    fp = rect_float;
    for (y = 0; y < h; y++) {
      x = 0;
#if defined(__ARM_NEON)
      for (; x + 4 <= w; x += 4, fp += 16) {
        unpremultiply_rect_float_neon4(fp);
      }
#endif
      for (; x < w; x++, fp += 4) {
        val = fp[3] != 0.0f ? 1.0f / fp[3] : 1.0f;
        fp[0] = fp[0] * val;
        fp[1] = fp[1] * val;
        fp[2] = fp[2] * val;
      }
    }
  }
}

void IMB_unpremultiply_alpha(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  if (uchar *byte_data = ibuf->byte_data_for_write()) {
    IMB_unpremultiply_rect(byte_data, ibuf->color_mode, ibuf->x, ibuf->y);
  }

  if (float *float_data = ibuf->float_data_for_write()) {
    IMB_unpremultiply_rect_float(float_data, ibuf->channels, ibuf->x, ibuf->y);
  }
}

}  // namespace blender
