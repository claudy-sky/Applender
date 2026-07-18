/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_bit_bool_conversion.hh"
#include "BLI_simd.hh"

namespace blender::bits {

static inline void or_mask_into_bits(MutableBitSpan r_bits,
                                     const int64_t bit_offset,
                                     const BitInt mask,
                                     const int mask_bits)
{
  BLI_assert(mask_bits > 0 && mask_bits <= BitsPerInt);
  const int64_t bit_index = r_bits.bit_range().start() + bit_offset;
  const int start_bit_in_int = bit_index & BitIndexMask;
  BitInt *start_bit_int = int_containing_bit(r_bits.data(), bit_index);
  *start_bit_int |= mask << start_bit_in_int;

  if (start_bit_in_int + mask_bits > BitsPerInt) {
    start_bit_int[1] |= mask >> (BitsPerInt - start_bit_in_int);
  }
}

template<typename ByteToBit>
bool or_bytes_into_bits(const Span<char> bytes,
                        MutableBitSpan r_bits,
                        const int64_t allowed_overshoot,
                        const ByteToBit &byte_to_bit)
{
  BLI_assert(r_bits.size() >= bytes.size());
  if (bytes.is_empty()) {
    return false;
  }

  int64_t byte_i = 0;
  const char *bytes_ = bytes.data();

  bool any_true = false;

/* Conversion from bytes to bits can be way faster with intrinsics. That's because instead of
 * processing one element at a time, we can process 16 at once. */
#if BLI_HAVE_ARM_NEON || BLI_HAVE_SSE2
  int64_t iteration_end = bytes.size();
  if (iteration_end % 16 > 0) {
    if (allowed_overshoot >= 16) {
      iteration_end = (iteration_end + 16) & ~15;
    }
  }
  /* A BitInt-sized block avoids four dependent read-modify-write operations on the output. */
  for (; byte_i + BitsPerInt <= iteration_end; byte_i += BitsPerInt) {
    BitInt is_true_mask = byte_to_bit.simd_chunk(bytes_ + byte_i);
    is_true_mask |= BitInt(byte_to_bit.simd_chunk(bytes_ + byte_i + 16)) << 16;
    is_true_mask |= BitInt(byte_to_bit.simd_chunk(bytes_ + byte_i + 32)) << 32;
    is_true_mask |= BitInt(byte_to_bit.simd_chunk(bytes_ + byte_i + 48)) << 48;
    if (is_true_mask != 0) {
      any_true = true;
      or_mask_into_bits(r_bits, byte_i, is_true_mask, BitsPerInt);
    }
  }
  /* Iterate over chunks of bytes. */
  for (; byte_i + 16 <= iteration_end; byte_i += 16) {
    const uint16_t is_true_mask = byte_to_bit.simd_chunk(bytes_ + byte_i);
    if (is_true_mask != 0) {
      any_true = true;
      or_mask_into_bits(r_bits, byte_i, is_true_mask, 16);
    }
  }
#endif

  /* Process remaining bytes. */
  for (; byte_i < bytes.size(); byte_i++) {
    if (byte_to_bit.single(bytes_[byte_i])) {
      r_bits[byte_i].set();
      any_true = true;
    }
  }
  return any_true;
}

struct BoolToBit {
  static bool single(const char c)
  {
    return bool(c);
  }

#if BLI_HAVE_ARM_NEON
  static uint16_t simd_chunk(const char *bytes)
  {
    /* Avoid translating an SSE movemask through sse2neon. */
    const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t *>(bytes));
    /* Convert non-zero bytes to 0xff, then weight each lane with its output bit. The two
     * horizontal adds cannot overflow because the weights in each half sum to 255. */
    alignas(16) static constexpr uint8_t bit_weights[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t is_true = vmvnq_u8(vceqq_u8(chunk, vdupq_n_u8(0)));
    const uint8x16_t weighted = vandq_u8(is_true, vld1q_u8(bit_weights));
    const uint16_t low_mask = vaddv_u8(vget_low_u8(weighted));
    const uint16_t high_mask = vaddv_u8(vget_high_u8(weighted));
    return low_mask | (high_mask << 8);
  }
#elif BLI_HAVE_SSE2
  static uint16_t simd_chunk(const char *bytes)
  {
    const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes));
    const __m128i zero_bytes = _mm_set1_epi8(0);
    /* Compare them all against zero. The result is a mask of the form [0x00, 0xff, 0xff, ...]. */
    const __m128i is_false_byte_mask = _mm_cmpeq_epi8(chunk, zero_bytes);
    /* Compress the byte-mask into a bit mask. This takes one bit from each byte. */
    const uint16_t is_false_mask = _mm_movemask_epi8(is_false_byte_mask);
    /* Now we have a bit mask where each bit corresponds to an input byte. */
    const uint16_t is_true_mask = ~is_false_mask;
    return is_true_mask;
  }
#endif
};

bool or_bools_into_bits(const Span<bool> bools,
                        MutableBitSpan r_bits,
                        const int64_t allowed_overshoot)
{
  return or_bytes_into_bits(bools.cast<char>(), r_bits, allowed_overshoot, BoolToBit());
}

}  // namespace blender::bits
