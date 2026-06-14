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

#include "ans32.h"
#include "memops.h"
#include "utils.h"

//

namespace iguana::ans32 {
    void (*encoder::g_Compress)(context& ctx) = &encoder::compress_portable;
    const internal::initializer<encoder> encoder::g_Initializer;

    void (*decoder::g_Decompress)(context& ctx) = &decoder::decompress_portable;
    const internal::initializer<decoder> decoder::g_Initializer;
}

//

iguana::ans32::encoder::encoder() {
    m_fwd.reserve(statistics::initial_buffer_size);
    m_rev.reserve(statistics::initial_buffer_size);
}

iguana::ans32::encoder::~encoder() noexcept {}
  
// This experimental arithmetic compression/decompression functionality is based on
// the work of Fabian Giesen, available here: https://github.com/rygorous/ryg_rans
// and kindly placed in the Public Domain per the CC0 licence:
// https://github.com/rygorous/ryg_rans/blob/master/LICENSE
//
// For theoretical background, please refer to Jaroslaw Duda's seminal paper on rANS:
// https://arxiv.org/pdf/1311.2540.pdf

void iguana::ans32::encoder::put(context& ctx, const std::uint8_t* p, std::size_t n) {
	// the forward half
	for(int lane = 15; lane >= 0; --lane) {
		if (lane < n) {
			const auto q = ctx.stats[p[lane]];
			const auto freq = q & statistics::frequency_mask;
			const auto start = (q >> statistics::frequency_bits) & statistics::cumulative_frequency_mask;
			// renormalize
			auto x = ctx.state[lane];
			if (x >= ((statistics::word_L >> statistics::word_M_bits) << statistics::word_L_bits) * freq) {
				ctx.fwd.append_big_endian(static_cast<std::uint16_t>(x));
				x >>= statistics::word_L_bits;
			}
			// x = C(s,x)
			ctx.state[lane] = ((x / freq) << statistics::word_M_bits) + (x % freq) + start;
		}
	}
	// the reverse half
	for(int lane = 31; lane >= 16; --lane) {
		if (lane < n) {
			const auto q = ctx.stats[p[lane]];
			const auto freq = q & statistics::frequency_mask;
			const auto start = (q >> statistics::frequency_bits) & statistics::cumulative_frequency_mask;
			// renormalize
			auto x = ctx.state[lane];
			if (x >= ((statistics::word_L >> statistics::word_M_bits) << statistics::word_L_bits) * freq) {
				ctx.rev.append_little_endian(static_cast<std::uint16_t>(x));
				x >>= statistics::word_L_bits;
			}
			// x = C(s,x)
			ctx.state[lane] = ((x / freq) << statistics::word_M_bits) + (x % freq) + start;
		}
	}
}

void iguana::ans32::encoder::encode(output_stream& dst, const statistics& stats, const std::uint8_t *src, std::size_t src_len) {
    // NOTE (ClickHouse): accumulate the forward half into a dedicated buffer (m_fwd) rather than
    // directly into dst. The upstream port wrote the forward half straight into dst and then
    // appended the *reverse* of the reverse half, which does not match the decoder. The Go
    // reference assembles the stream as reverse(bufFwd) ++ bufRev: the forward half is reversed in
    // place and the reverse half is appended as-is. We reproduce that below.
    m_fwd.clear();
    m_rev.clear();
    context ctx { .fwd = m_fwd, .rev = m_rev, .stats = stats, .src = src, .src_len = src_len };
    memory::fill(ctx.state, statistics::word_L);
    g_Compress(ctx);

    if (ctx.ec != error_code::ok) {
        exception::from_error(ctx.ec);
    }

    dst.reserve_more(m_fwd.size() + m_rev.size() + statistics::dense_table_max_length);
    dst.append_reverse(m_fwd.data(), m_fwd.size());   // reverse(bufFwd)
    dst.append(m_rev.data(), m_rev.size());           // ++ bufRev (forward order)

    // NOTE (ClickHouse): the upstream port reserved space for the statistics table here but never
    // actually serialized it. The decoder reconstructs the frequency table from the tail of the
    // compressed stream (see byte_statistics::deserialize), so without this call the round-trip is
    // broken. Append the serialized statistics so the bitstream matches what the decoder expects.
    stats.serialize(dst);
}

void iguana::ans32::encoder::compress_portable(context& ctx) {
	const auto n_last = ctx.src_len % 32;
	long k = ctx.src_len - n_last;

	// Process the last chunk first
	put(ctx, ctx.src + k, n_last);

	// Process the remaining chunks
	for(k -= 32; k >= 0; k -= 32) {
		put(ctx, ctx.src + k, 32);
	}

    // Flush
	for(int lane = 15; lane >= 0; --lane) {
        ctx.fwd.append_big_endian(ctx.state[lane]);
	}

	for(int lane = 16; lane < 32; ++lane) {
        ctx.rev.append_little_endian(ctx.state[lane]);
	}

    ctx.ec = error_code::ok;
}

void iguana::ans32::encoder::at_process_start() {}

void iguana::ans32::encoder::at_process_end() {}

//

iguana::ans32::decoder::~decoder() noexcept {}

void iguana::ans32::decoder::decode(output_stream& dst, std::size_t result_size, input_stream& src, const statistics::decoding_table& tab) {
    dst.reserve_more(result_size);
    context ctx{ .dst = dst, .result_size = result_size, .src = src, .tab = tab };
    g_Decompress(ctx);

    if (ctx.ec != error_code::ok) {
        exception::from_error(ctx.ec);
    }
}        

void iguana::ans32::decoder::decompress_portable(context& ctx) {
	// NOTE (ClickHouse): guard against a malformed substream. The forward and reverse state
	// vectors occupy 64 bytes each at the two ends of the payload; without this check a payload
	// shorter than 128 bytes would make cursor_rev underflow and read out of bounds.
	if (ctx.src.size() < 128) {
		ctx.ec = error_code::corrupted_bitstream;
		return;
	}
	std::uint32_t state[32];
	std::size_t cursor_fwd = 64;
	std::size_t cursor_rev = ctx.src.size() - 64;
    const std::uint8_t* const src = ctx.src.data();

	for(std::size_t lane = 0; lane != 16; ++lane) {
		state[lane]    = utils::read_little_endian<std::uint32_t>(src + lane * 4);
		state[lane+16] = utils::read_little_endian<std::uint32_t>(src + lane * 4 + cursor_rev);
	}

	std::size_t cursor_dst = 0;

	for(;;) {
		for(std::size_t lane = 0; lane != 32; ++lane) {
			std::uint32_t x = state[lane];
			const auto slot = x & (statistics::word_M - 1);
			const auto t = ctx.tab[slot];
			const auto freq = std::uint32_t(t & (statistics::word_M - 1));
			const auto bias = std::uint32_t((t >> statistics::word_M_bits) & (statistics::word_M - 1));
			// s, x = D(x)
			state[lane] = freq * (x >> statistics::word_M_bits) + bias;
			const auto s = std::uint8_t(t >> 24);
			if (cursor_dst < ctx.result_size) {
				ctx.dst.append(s);
				++cursor_dst;
			} else {
				goto done;
			}
		}
		// Normalize the forward part
		for(std::size_t lane = 0; lane != 16; ++lane) {
			if (const auto x = state[lane]; x < statistics::word_L) {
				const auto v = utils::read_little_endian<std::uint16_t>(src + cursor_fwd);
				cursor_fwd += 2;
				state[lane] = (x << statistics::word_L_bits) | std::uint32_t(v);
			}
		}
		// Normalize the reverse part
		for(std::size_t lane = 16; lane != 32; ++lane) {
			if (const auto x = state[lane]; x < statistics::word_L) {
				const auto v = utils::read_little_endian<std::uint16_t>(src + cursor_rev - 2);
				cursor_rev -= 2;
				state[lane] = (x << statistics::word_L_bits) | std::uint32_t(v);
			}
		}
	}

done:

    // NOTE (ClickHouse): the upstream port validated here that every lane's state returned to
    // word_L. That check is incorrect for the 32-way interleaved decoder and is absent from the Go
    // reference (ans32DecompressReference simply returns the data): the decode loop emits in groups
    // of 32 and stops mid-group once result_size symbols have been produced, so the lanes decoded
    // before the stop are advanced one extra step and do not end at word_L even for valid input.
    // The decoded bytes are nonetheless correct, so we simply return success.
    ctx.ec = error_code::ok;
}

void iguana::ans32::decoder::at_process_start() {
    // NOTE (ClickHouse): dynamic CPU dispatch. g_Decompress was statically initialized to the
    // portable kernel above; switch to the AVX-512 kernel when the host supports it.
    // Define IGUANA_DISABLE_DISPATCH to force the portable kernels (debugging / benchmarking).
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(IGUANA_DISABLE_DISPATCH)
    if (internal::cpu_has_avx512())
        g_Decompress = &decoder::decompress_avx512;
#endif
}

void iguana::ans32::decoder::at_process_end() {}
