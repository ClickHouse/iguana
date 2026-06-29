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

// NOTE (ClickHouse): AArch64 NEON implementation of the Iguana structural sequence decoder. The
// token parsing mirrors decoder::decompress_portable exactly (keep the two in sync); the difference
// is the match copy, which uses 16-byte NEON loads/stores instead of a byte-at-a-time loop. Literal
// runs go through output_stream::append, which is a bulk memcpy. Compiled only on AArch64 (see the
// CMake glue) and dispatched at process start by decoder::at_process_start.

#include "decoder.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace iguana {

namespace {

// Mirrors the constants in decoder.cpp.
constexpr std::uint32_t neon_literal_len_bits    = 3;
constexpr std::uint32_t neon_mm_long_offsets     = 16;
constexpr std::uint32_t neon_max_short_lit_len   = 7;
constexpr std::uint32_t neon_max_short_match_len = 15;
constexpr std::uint32_t neon_last_long_offset    = 31;

// Append dst[offs : offs+len] to dst with overlapped (self-referential) copy semantics, using
// 16-byte NEON copies when the match distance is >= 16 (so each 16-byte source window is fully
// behind the current write position and thus already materialized). For distances < 16 the bytes
// must be replicated one at a time. The output buffer is guaranteed not to reallocate here because
// decoder::decode() reserves the whole uncompressed size up front.
void neon_wild_copy(output_stream& dst, std::size_t offs, std::size_t len)
{
    const std::size_t old_size = dst.size();
    // NOTE (ClickHouse): the match offset comes from the untrusted offset substreams. The caller
    // computes it as (output size - match distance) truncated to 32 bits, which wraps to a large
    // value when the distance exceeds the output produced so far. Reading from such an offset would
    // be an out-of-bounds read (and `distance` below would underflow), so reject it. A valid match
    // always references already-produced output (offs < old_size). Mirrors decoder::wild_copy.
    if (offs >= old_size)
        throw corrupted_bitstream_exception("Iguana match offset points outside the decompressed output");
    std::uint8_t * const w = dst.claim(len);   // [old_size, old_size+len); no reallocation
    const std::uint8_t * const src = dst.data() + offs;
    const std::size_t distance = old_size - offs;

    std::size_t i = 0;
#if defined(__aarch64__)
    if (distance >= 16)
    {
        for (; i + 16 <= len; i += 16)
            vst1q_u8(w + i, vld1q_u8(src + i));
    }
#endif
    for (; i < len; ++i)
        w[i] = src[i];
}

} // anonymous namespace

void decoder::decompress_neon(context& ctx)
{
    // [0_MMMM_LLL] - 16-bit offset, 4-bit match length (4-15+), 3-bit literal length (0-7+)
    // [1_MMMM_LLL] -   last offset, 4-bit match length (0-15+), 3-bit literal length (0-7+)
    // flag 31      - 24-bit offset,        match length (47+),    no literal length
    // flag 0-30    - 24-bit offset,  31 match lengths (16-46),    no literal length
    auto last_offs = ctx.last_offset;

    while (!ctx.streams[substream::tokens].empty())
    {
        std::uint32_t match_len = 0;
        const auto token = ctx.streams[substream::tokens].fetch8(ctx.ec);
        if (ctx.ec != error_code::ok)
            return;

        if (token >= 32)
        {
            auto lit_len = std::uint32_t(token & neon_max_short_lit_len);
            if (lit_len == neon_max_short_lit_len)
            {
                const auto val = ctx.streams[substream::var_lit_len].fetch_var_uint(ctx.ec);
                if (ctx.ec != error_code::ok)
                    return;
                lit_len = val + neon_max_short_lit_len;
            }
            if (lit_len > 0)
            {
                if (const auto seq = ctx.streams[substream::literals].fetch_sequence(lit_len, ctx.ec); ctx.ec != error_code::ok)
                    return;
                else
                    ctx.dst.append(seq);
            }

            if ((token & 0x80) == 0)
            {
                const auto new_offs = ctx.streams[substream::offset16].fetch16(ctx.ec);
                if (ctx.ec != error_code::ok)
                    return;
                last_offs = -std::int64_t(new_offs);
            }

            match_len = (token >> neon_literal_len_bits) & neon_max_short_match_len;
            if (match_len == neon_max_short_match_len)
            {
                const auto val = ctx.streams[substream::var_match_len].fetch_var_uint(ctx.ec);
                if (ctx.ec != error_code::ok)
                    return;
                match_len = val + neon_max_short_match_len;
            }
        }
        else if (token < neon_last_long_offset)
        {
            match_len = token + neon_mm_long_offsets;
            const auto x = ctx.streams[substream::offset24].fetch24(ctx.ec);
            if (ctx.ec != error_code::ok)
                return;
            last_offs = -std::int64_t(x);
        }
        else
        {
            const auto val = ctx.streams[substream::var_match_len].fetch_var_uint(ctx.ec);
            if (ctx.ec != error_code::ok)
                return;
            match_len = val + neon_last_long_offset + neon_mm_long_offsets;
            const auto x = ctx.streams[substream::offset24].fetch24(ctx.ec);
            if (ctx.ec != error_code::ok)
                return;
            last_offs = -std::int64_t(x);
        }

        const std::uint32_t match = ctx.dst.size() + last_offs;
        neon_wild_copy(ctx.dst, match, match_len);
    }

    if (const auto remainder_len = ctx.streams[substream::literals].remaining(); remainder_len > 0)
    {
        const auto seq = ctx.streams[substream::literals].fetch_sequence(remainder_len, ctx.ec);
        if (ctx.ec != error_code::ok)
            return;
        else
            ctx.dst.append(seq);
    }

    ctx.last_offset = last_offs;
    ctx.ec = error_code::ok;
}

} // namespace iguana
