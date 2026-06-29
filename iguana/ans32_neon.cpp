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

// NOTE (ClickHouse): AArch64 NEON implementation of the 32-way interleaved rANS decoder. The 32
// lanes are processed as eight uint32x4 vectors; the rANS state arithmetic is vectorized, while the
// table lookup (NEON has no gather) and the renormalization (NEON has no compress/expand) are done
// scalar per 4-lane group. Decoded symbols are written to a claimed raw buffer. Verified byte-exact
// against decompress_portable. Compiled only on AArch64 (see the CMake glue); dispatched at process
// start by ans32::decoder::at_process_start.

#include "ans32.h"
#include "ans_byte_statistics.h"

#include <arm_neon.h>
#include <cstring>

namespace iguana::ans32 {

namespace {

inline std::uint16_t load16(const std::uint8_t * p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }

} // anonymous namespace

void decoder::decompress_neon(context& ctx)
{
    using statistics = ans::byte_statistics;
    constexpr std::uint32_t M = statistics::word_M;       // 4096
    constexpr std::uint32_t L = statistics::word_L;       // 65536
    constexpr std::uint32_t MB = statistics::word_M_bits; // 12
    constexpr std::uint32_t LB = statistics::word_L_bits; // 16

    const std::size_t size = ctx.src.size();
    if (size < 128)
    {
        ctx.ec = error_code::corrupted_bitstream;
        return;
    }

    const std::uint8_t * const src = ctx.src.data();
    const std::uint32_t * const tab = ctx.tab;
    std::uint8_t * const out = ctx.dst.claim(ctx.result_size);
    const std::size_t result_size = ctx.result_size;

    // sv[0..3] = forward lanes 0..15, sv[4..7] = reverse lanes 16..31 (each holds 4 lanes).
    uint32x4_t sv[8];
    for (int g = 0; g < 4; ++g)
        sv[g] = vld1q_u32(reinterpret_cast<const std::uint32_t *>(src + g * 16));
    std::size_t cursor_rev = size - 64;
    for (int g = 0; g < 4; ++g)
        sv[g + 4] = vld1q_u32(reinterpret_cast<const std::uint32_t *>(src + cursor_rev + g * 16));
    std::size_t cursor_fwd = 64;

    const uint32x4_t mmask = vdupq_n_u32(M - 1);
    std::size_t cursor = 0;

    for (;;)
    {
        std::uint8_t syms[32];
        for (int g = 0; g < 8; ++g)
        {
            const uint32x4_t slot = vandq_u32(sv[g], mmask);
            const std::uint32_t tt[4] = {
                tab[vgetq_lane_u32(slot, 0)], tab[vgetq_lane_u32(slot, 1)],
                tab[vgetq_lane_u32(slot, 2)], tab[vgetq_lane_u32(slot, 3)] };
            const uint32x4_t t = vld1q_u32(tt);
            const uint32x4_t freq = vandq_u32(t, mmask);
            const uint32x4_t bias = vandq_u32(vshrq_n_u32(t, MB), mmask);
            sv[g] = vaddq_u32(vmulq_u32(freq, vshrq_n_u32(sv[g], MB)), bias);
            // symbol byte = t >> 24, narrowed to 4 bytes
            const uint16x4_t s16 = vmovn_u32(vshrq_n_u32(t, 24));
            const std::uint16_t tmp[4] = { vget_lane_u16(s16, 0), vget_lane_u16(s16, 1),
                                           vget_lane_u16(s16, 2), vget_lane_u16(s16, 3) };
            syms[g * 4 + 0] = std::uint8_t(tmp[0]);
            syms[g * 4 + 1] = std::uint8_t(tmp[1]);
            syms[g * 4 + 2] = std::uint8_t(tmp[2]);
            syms[g * 4 + 3] = std::uint8_t(tmp[3]);
        }

        if (cursor + 32 <= result_size)
        {
            std::memcpy(out + cursor, syms, 32);
            cursor += 32;
        }
        else
        {
            std::memcpy(out + cursor, syms, result_size - cursor);
            break;
        }

        // Forward renormalization (lanes 0..15, words consumed forward).
        for (int g = 0; g < 4; ++g)
        {
            std::uint32_t s[4];
            vst1q_u32(s, sv[g]);
            for (int l = 0; l < 4; ++l)
                if (s[l] < L)
                {
                    // NOTE (ClickHouse): bound the forward renorm read against the substream so a
                    // malformed bitstream cannot read past the end of the buffer.
                    if (cursor_fwd > size - 2) { ctx.ec = error_code::corrupted_bitstream; return; }
                    s[l] = (s[l] << LB) | load16(src + cursor_fwd);
                    cursor_fwd += 2;
                }
            sv[g] = vld1q_u32(s);
        }
        // Reverse renormalization (lanes 16..31, words consumed backward).
        for (int g = 4; g < 8; ++g)
        {
            std::uint32_t s[4];
            vst1q_u32(s, sv[g]);
            for (int l = 0; l < 4; ++l)
                if (s[l] < L)
                {
                    // NOTE (ClickHouse): bound the backward renorm read against the substream so a
                    // malformed bitstream cannot read before the buffer (cursor_rev wraps on underflow).
                    if (cursor_rev < 2) { ctx.ec = error_code::corrupted_bitstream; return; }
                    s[l] = (s[l] << LB) | load16(src + cursor_rev - 2);
                    cursor_rev -= 2;
                }
            sv[g] = vld1q_u32(s);
        }
    }

    ctx.ec = error_code::ok;
}

} // namespace iguana::ans32
