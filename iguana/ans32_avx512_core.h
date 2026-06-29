// Copyright 2023 Sneller, Inc.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

// NOTE (ClickHouse): freestanding AVX-512 core of the 32-way interleaved rANS decoder. It depends
// only on <stdint.h> and the AVX-512 intrinsics (no C++ standard library), so the exact same code
// that ships in the codec can be compiled into a standalone validation harness and run under
// qemu-x86_64 (the development host is AArch64 and cannot execute AVX-512 natively). Ported from the
// Go reference assembly (ans32DecompressAVX512Generic) and verified byte-exact against the portable
// decoder. Uses AVX-512 F + BW + VL only (no VBMI2).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <immintrin.h>

namespace iguana_avx512 {

// rANS parameters fixed by the bitstream format (== ans::byte_statistics word_*).
inline constexpr uint32_t ANS_M_BITS = 12;
inline constexpr uint32_t ANS_L_BITS = 16;
inline constexpr uint32_t ANS_M      = uint32_t(1) << ANS_M_BITS;  // 4096
inline constexpr uint32_t ANS_L      = uint32_t(1) << ANS_L_BITS;  // 65536

// Pack the symbol byte (high byte of each gathered table entry) of the 16 low lanes and the 16 high
// lanes into 32 contiguous bytes [lane0 .. lane31].
inline __m256i ans32_extract_symbols(__m512i tab_lo, __m512i tab_hi)
{
    const __m128i s_lo = _mm512_cvtepi32_epi8(_mm512_srli_epi32(tab_lo, 24));
    const __m128i s_hi = _mm512_cvtepi32_epi8(_mm512_srli_epi32(tab_hi, 24));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(s_lo), s_hi, 1);
}

// Decode result_size bytes from the 32-way rANS payload [src, src+size) using the decoding table
// tab[ANS_M], writing to out[result_size]. Returns false on a malformed (too-short) payload.
inline bool ans32_decode(const uint8_t * src, size_t size, const uint32_t * tab, size_t result_size, uint8_t * out)
{
    // Forward and reverse state vectors occupy 64 bytes each at the two ends of the payload.
    if (size < 128)
        return false;

    __m512i state_lo = _mm512_loadu_si512(reinterpret_cast<const void *>(src));          // lanes 0..15
    const uint8_t * cursor_rev = src + size - 64;
    __m512i state_hi = _mm512_loadu_si512(reinterpret_cast<const void *>(cursor_rev));    // lanes 16..31
    const uint8_t * cursor_fwd = src + 64;

    const __m512i mmask = _mm512_set1_epi32(int(ANS_M - 1));   // 0x0fff
    const __m512i lvec  = _mm512_set1_epi32(int(ANS_L));       // 0x10000
    // Reverse 16 16-bit words (result[i] = src[15 - i]); the reverse half is read high-to-low.
    const __m256i rev_word_idx = _mm256_setr_epi16(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);

    size_t produced = 0;

    while (result_size - produced > 32)
    {
        const __m512i tab_lo = _mm512_i32gather_epi32(_mm512_and_epi32(state_lo, mmask), tab, 4);
        const __m512i tab_hi = _mm512_i32gather_epi32(_mm512_and_epi32(state_hi, mmask), tab, 4);

        _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + produced), ans32_extract_symbols(tab_lo, tab_hi));
        produced += 32;

        // state = freq * (state >> M_bits) + bias
        state_lo = _mm512_add_epi32(
            _mm512_mullo_epi32(_mm512_and_epi32(tab_lo, mmask), _mm512_srli_epi32(state_lo, ANS_M_BITS)),
            _mm512_and_epi32(_mm512_srli_epi32(tab_lo, ANS_M_BITS), mmask));
        state_hi = _mm512_add_epi32(
            _mm512_mullo_epi32(_mm512_and_epi32(tab_hi, mmask), _mm512_srli_epi32(state_hi, ANS_M_BITS)),
            _mm512_and_epi32(_mm512_srli_epi32(tab_hi, ANS_M_BITS), mmask));

        // Forward renormalization: every lane with state < L consumes the next 16-bit word.
        // NOTE (ClickHouse): the renorm step speculatively loads a full 32-byte block at cursor_fwd
        // before consuming only the needed words. Bound it against the substream so a malformed
        // bitstream cannot read past the end of the buffer. A valid stream consumes at most
        // (size - 128) renorm bytes split between the two halves, so cursor_fwd + 32 never exceeds
        // src + size here and this guard cannot reject a well-formed payload.
        if (cursor_fwd + 32 > src + size)
            return false;
        const __m512i fwd_words = _mm512_cvtepu16_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(cursor_fwd)));
        const __mmask16 k_lo = _mm512_cmplt_epu32_mask(state_lo, lvec);
        state_lo = _mm512_mask_or_epi32(state_lo, k_lo,
            _mm512_slli_epi32(state_lo, ANS_L_BITS), _mm512_maskz_expand_epi32(k_lo, fwd_words));
        cursor_fwd += 2 * size_t(_mm_popcnt_u32(k_lo));

        // Reverse renormalization: words read from high to low addresses, hence reversed first.
        // NOTE (ClickHouse): as for the forward half, bound the speculative 32-byte block load at
        // cursor_rev - 32 against the start of the substream. A valid stream keeps cursor_rev - 32
        // at or above src, so this guard only fires on malformed input.
        if (cursor_rev < src + 32)
            return false;
        const __m256i rev_raw = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cursor_rev - 32));
        const __m512i rev_words = _mm512_cvtepu16_epi32(_mm256_permutexvar_epi16(rev_word_idx, rev_raw));
        const __mmask16 k_hi = _mm512_cmplt_epu32_mask(state_hi, lvec);
        state_hi = _mm512_mask_or_epi32(state_hi, k_hi,
            _mm512_slli_epi32(state_hi, ANS_L_BITS), _mm512_maskz_expand_epi32(k_hi, rev_words));
        cursor_rev -= 2 * size_t(_mm_popcnt_u32(k_hi));
    }

    // Final partial chunk of 1..32 symbols: decode and masked-store, no renormalization needed.
    if (const size_t remaining = result_size - produced; remaining > 0)
    {
        const __m512i tab_lo = _mm512_i32gather_epi32(_mm512_and_epi32(state_lo, mmask), tab, 4);
        const __m512i tab_hi = _mm512_i32gather_epi32(_mm512_and_epi32(state_hi, mmask), tab, 4);
        const __mmask32 store_mask = (remaining >= 32)
            ? __mmask32(0xffffffffu)
            : __mmask32((1u << remaining) - 1u);
        _mm256_mask_storeu_epi8(out + produced, store_mask, ans32_extract_symbols(tab_lo, tab_hi));
    }

    return true;
}

} // namespace iguana_avx512
