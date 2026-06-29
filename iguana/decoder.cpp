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

#include <memory>
#include <utility>
#include <stdexcept>
#include "decoder.h"
#include "command.h"
#include "entropy.h"
#include "ans1.h"
#include "ans32.h"
#include "ans_nibble.h"

//

namespace iguana {
    void (*decoder::g_Decompress)(context& ctx) = &decoder::decompress_portable;
    const internal::initializer<decoder> decoder::g_Initializer;

    //

    static constexpr const std::uint32_t match_len_bits      = 4;
    static constexpr const std::uint32_t literal_len_bits    = 3;
    static constexpr const std::uint32_t mm_long_offsets     = 16;
    static constexpr const std::int64_t  init_last_offset    = 0;
    static constexpr const std::uint32_t max_short_lit_len   = 7;
    static constexpr const std::uint32_t max_short_match_len = 15;
    static constexpr const std::uint32_t last_long_offset    = 31;

    // We'd like to allow 64-byte loads at the final byte offset
    // for each of the streams, so we need (64 - 1) bytes of valid memory
    // past the end of the buffer.
    static constexpr const std::size_t pad_size = (64 - 1);
}

//

iguana::decoder::~decoder() noexcept {}

void iguana::decoder::decode(output_stream& dst, input_stream& src) {
    ssize_t cursor = src.size();
    if (cursor == 0) {
        throw out_of_input_data_exception();
    }

    --cursor;
    const auto* const p_data = src.data();
	const std::uint64_t uncompressed_len = read_control_var_uint(p_data, cursor);

	if (uncompressed_len == 0) {
		return;
	}

    dst.reserve_more(uncompressed_len);
    decompress(dst, p_data, src.size(), uncompressed_len, cursor);
}

std::uint64_t iguana::decoder::read_control_var_uint(const std::uint8_t* src, ssize_t& cursor) {
	std::uint64_t r = 0;
	while(cursor >= 0) {
		const std::uint8_t v = src[cursor--];
		r = (r << 7) | std::uint64_t(v & 0x7f);
		if ((v & 0x80) != 0) {
            return r;
		}
	}

    throw out_of_input_data_exception();
}

void iguana::decoder::decompress(output_stream& dst, const std::uint8_t* const src, std::size_t src_size, std::uint64_t uncompressed_len, ssize_t& ctrl_cursor) {
    // NOTE (ClickHouse): the upstream port left an IGUANA_UNIMPLEMENTED marker at the top of this
    // function, which aborted before reaching the command dispatch loop below. The copy_raw,
    // decode_ans32 and decode_ans1 command handlers are fully implemented, so the marker is removed
    // to enable the entropy-only decoding path used by the Iguana codec.

    context ctx{ .dst = dst, .last_offset = 0 };

    // NOTE (ClickHouse): every command reads its payload from the forward-growing data region at the
    // front of the same buffer the control stream is consumed from (backwards). Each payload length
    // comes from the untrusted control stream, so it must be bounded against the supplied input before
    // the data cursor is advanced or the bytes are read; otherwise a malformed stream can make
    // copy_raw, an entropy substream, or a verbatim substream read past the end of the compressed
    // buffer. require_data(n) throws unless [data_cursor, data_cursor + n) stays within src_size.
    const auto require_data = [src_size](std::uint64_t data_cursor, std::uint64_t n)
    {
        if (data_cursor > src_size || n > std::uint64_t(src_size) - data_cursor)
            throw out_of_input_data_exception();
    };

	// Fetch the header

	for(std::uint64_t data_cursor = 0;;) {
		if (ctrl_cursor < 0) {
            throw out_of_input_data_exception();
		}
		const std::uint8_t cmd = src[ctrl_cursor--];

		switch (static_cast<command>(cmd & command_mask)) {
            case command::copy_raw: {
                const std::uint64_t n = read_control_var_uint(src, ctrl_cursor);
                require_data(data_cursor, n);
                dst.append(src + data_cursor, n);
                data_cursor += n;
            } break;

            case command::decode_ans32: {
                const std::uint64_t len_uncompressed = read_control_var_uint(src, ctrl_cursor);
                const std::uint64_t len_compressed = read_control_var_uint(src, ctrl_cursor);
                require_data(data_cursor, len_compressed);

                {   typename ans32::decoder::statistics::decoding_table ans_tab;
                    input_stream is{src + data_cursor, std::size_t(len_compressed)};
                    data_cursor += len_compressed;
                    // Recover the ANS decoding table from the input stream                
                    ans32::decoder::statistics{is}.build_decoding_table(ans_tab);

                    // Decode the compressed content
                    ans32::decoder{}.decode(dst, static_cast<std::size_t>(len_uncompressed), is, ans_tab);
                }
            } break;

		case command::decode_ans1: {
                const std::uint64_t len_uncompressed = read_control_var_uint(src, ctrl_cursor);
                const std::uint64_t len_compressed = read_control_var_uint(src, ctrl_cursor);
                require_data(data_cursor, len_compressed);

                 {  ans1::decoder::statistics::decoding_table ans_tab;
                    input_stream is{src + data_cursor, std::size_t(len_compressed)};
                    data_cursor += len_compressed;
                    // Recover the ANS decoding table from the input stream                
                    ans1::decoder::statistics{is}.build_decoding_table(ans_tab);

                    // Decode the compressed content
                    ans1::decoder{}.decode(dst, static_cast<std::size_t>(len_uncompressed), is, ans_tab);
                }
            } break;

		case command::decode_ans_nibble: {
                const std::uint64_t len_uncompressed = read_control_var_uint(src, ctrl_cursor);
                const std::uint64_t len_compressed = read_control_var_uint(src, ctrl_cursor);
                require_data(data_cursor, len_compressed);

                {   ans_nibble::decoder::statistics::decoding_table ans_tab;
                    input_stream is{src + data_cursor, std::size_t(len_compressed)};
                    data_cursor += len_compressed;
                    // Recover the ANS decoding table from the input stream                
                    ans_nibble::decoder::statistics{is}.build_decoding_table(ans_tab);

                    // Decode the compressed content
                    ans_nibble::decoder{}.decode(dst, static_cast<std::size_t>(len_uncompressed), is, ans_tab);
                }
            } break;

		case command::decode_iguana: {
            // NOTE (ClickHouse): completed from the Go reference (decoder.go). Each substream is
            // either stored verbatim or entropy-coded; entropy-coded substreams are decoded into
            // their own buffer (entropy_storage) which must outlive the g_Decompress call below.
            // The portable sequence decoder reads the substreams through bounds-checked fetches, so
            // no SIMD-style end padding (padStream in the Go reference) is required.
            output_stream entropy_storage[substream::count];

			// Fetch the header byte
			if (ctrl_cursor < 0) {
                throw out_of_input_data_exception();
			}

            const std::uint64_t hdr = read_control_var_uint(src, ctrl_cursor);

			if (hdr == 0) {
                // No substream is entropy-coded: each is stored verbatim.
				for(std::size_t i = 0; i != substream::count; ++i) {
                    const std::uint64_t u_len = read_control_var_uint(src, ctrl_cursor);
                    require_data(data_cursor, u_len);
                    ctx.streams[i].set(src + data_cursor, std::size_t(u_len));
                    data_cursor += u_len;
				}
			} else {
				std::uint64_t u_lens[substream::count];
				for(std::size_t i = 0; i != substream::count; ++i) {
                    u_lens[i] = read_control_var_uint(src, ctrl_cursor);
				}

				for(std::size_t i = 0; i != substream::count; ++i) {
					const std::uint64_t u_len = u_lens[i];
					const auto em = static_cast<entropy_mode>((hdr >> (i * 4)) & 0x0f);
					if (em == entropy_mode::none) {
                        require_data(data_cursor, u_len);
                        ctx.streams[i].set(src + data_cursor, std::size_t(u_len));
                        data_cursor += u_len;
					} else {
                        const std::uint64_t c_len = read_control_var_uint(src, ctrl_cursor);
                        require_data(data_cursor, c_len);
                        input_stream is{src + data_cursor, std::size_t(c_len)};
                        data_cursor += c_len;
						switch(em) {
						case entropy_mode::ans32: {
                            ans32::decoder::statistics::decoding_table ans_tab;
                            ans32::decoder::statistics{is}.build_decoding_table(ans_tab);
                            ans32::decoder{}.decode(entropy_storage[i], std::size_t(u_len), is, ans_tab);
                        } break;

						case entropy_mode::ans1: {
                            ans1::decoder::statistics::decoding_table ans_tab;
                            ans1::decoder::statistics{is}.build_decoding_table(ans_tab);
                            ans1::decoder{}.decode(entropy_storage[i], std::size_t(u_len), is, ans_tab);
                        } break;

						case entropy_mode::ans_nibble: {
                            ans_nibble::decoder::statistics::decoding_table ans_tab;
                            ans_nibble::decoder::statistics{is}.build_decoding_table(ans_tab);
                            ans_nibble::decoder{}.decode(entropy_storage[i], std::size_t(u_len), is, ans_tab);
                        } break;

						default:
							throw corrupted_bitstream_exception("unrecognized entropy mode");
						}
                        ctx.streams[i].set(entropy_storage[i].data(), entropy_storage[i].size());
					}
				}
			}

			ctx.last_offset = init_last_offset;
            g_Decompress(ctx);

            if (ctx.ec != error_code::ok) {
                exception::from_error(ctx.ec);
            }
        } break;

		default:
            throw unrecognized_command_exception();
		}

		if ((cmd & last_command_marker) != 0) {
			return;
		}
	}
}

void iguana::decoder::decompress_portable(context& ctx) {
	// [0_MMMM_LLL] - 16-bit offset, 4-bit match length (4-15+), 3-bit literal length (0-7+)
	// [1_MMMM_LLL] -   last offset, 4-bit match length (0-15+), 3-bit literal length (0-7+)
	// flag 31      - 24-bit offset,        match length (47+),    no literal length
	// flag 0-30    - 24-bit offset,  31 match lengths (16-46),    no literal length

    auto last_offs = ctx.last_offset;

	// Main Loop : decode sequences
	while(!ctx.streams[substream::tokens].empty()) {
		//get literal length
		std::uint32_t match_len = 0;
		const auto token = ctx.streams[substream::tokens].fetch8(ctx.ec);
		if (ctx.ec != error_code::ok) {
			return;
		}

		if (token >= 32) {
			auto lit_len = std::uint32_t(token & max_short_lit_len);
			if (lit_len == max_short_lit_len) {
				const auto val = ctx.streams[substream::var_lit_len].fetch_var_uint(ctx.ec);
		        if (ctx.ec != error_code::ok) {
					return;
				}
				lit_len = val + max_short_lit_len;
			}
			if (lit_len > 0) {
				if (const auto seq = ctx.streams[substream::literals].fetch_sequence(lit_len, ctx.ec); ctx.ec != error_code::ok) {
					return;
				} else {
					ctx.dst.append(seq);
				}
			}

			// get offset
			if ((token & 0x80) == 0) {
				// [0_MMMM_LLL] - 16-bit offset, 4-bit match length (4-15+), 3-bit literal length (0-7+)
				const auto new_offs = ctx.streams[substream::offset16].fetch16(ctx.ec);
		        if (ctx.ec != error_code::ok) {
					return;
				}
				last_offs = -std::int64_t(new_offs);
			}

			// get matchlength
			match_len = (token >> literal_len_bits) & max_short_match_len;
			if (match_len == max_short_match_len) {
				const auto val = ctx.streams[substream::var_match_len].fetch_var_uint(ctx.ec);
		        if (ctx.ec != error_code::ok) {
					return;
				}
				match_len = val + max_short_match_len;
			}
		} else if (token < last_long_offset) {
			// token < 31
			match_len = token + mm_long_offsets;
			const auto x = ctx.streams[substream::offset24].fetch24(ctx.ec);
		    if (ctx.ec != error_code::ok) {
				return;
			}
			last_offs = -std::int64_t(x);
		} else {
			// token == 31
			const auto val = ctx.streams[substream::var_match_len].fetch_var_uint(ctx.ec);
		    if (ctx.ec != error_code::ok) {
				return;
			}
			match_len = val + last_long_offset + mm_long_offsets;
			const auto x = ctx.streams[substream::offset24].fetch24(ctx.ec);
		    if (ctx.ec != error_code::ok) {
				return;
			}
			last_offs = -std::int64_t(x);
		}
		const std::uint32_t match = ctx.dst.size() + last_offs;
		wild_copy(ctx.dst, match, match_len);
	}

	// last literals
	if (const auto remainder_len = ctx.streams[substream::literals].remaining(); remainder_len > 0) {
        const auto seq = ctx.streams[substream::literals].fetch_sequence(remainder_len, ctx.ec);
        if (ctx.ec != error_code::ok) {
			return;
		} else {
			ctx.dst.append(seq);
		}
	}

	// end of decoding
	ctx.last_offset = last_offs;
    ctx.ec = error_code::ok;
}

// Append dst[offs : offs+len] to dst, obeying overlapped (self-referential) copy semantics
// (port of iguanaWildCopy in the Go reference).
void iguana::decoder::wild_copy(output_stream& dst, std::size_t offs, std::size_t len) {
    // Ensure the buffer will not reallocate during the copy, so the source pointer stays valid
    // (the top-level decode() already reserves the full uncompressed size, so this is normally a
    // no-op). With a stable buffer, a byte-by-byte append reading from dst's own storage correctly
    // realizes both the non-overlapping and the overlapping cases: when offs+i reaches into the
    // freshly written region the byte read was produced earlier in this same loop, which is exactly
    // the LZ overlapped-copy semantics.
    // NOTE (ClickHouse): the match offset is derived from the untrusted token/offset substreams. The
    // caller computes it as (current output size - match distance) truncated to 32 bits, so a
    // malformed stream whose distance exceeds the output produced so far wraps around to a large
    // value. Reading from such an offset would be an out-of-bounds read of the output buffer, so
    // reject it instead. A valid match always references already-produced output (offs < size).
    if (offs >= dst.size())
        throw corrupted_bitstream_exception("Iguana match offset points outside the decompressed output");
    dst.reserve_more(len);
    const std::uint8_t* const base = dst.data();
    for (std::size_t i = 0; i < len; ++i)
        dst.append(base[offs + i]);
}

void iguana::decoder::at_process_start() {
    // NOTE (ClickHouse): dynamic dispatch for the structural sequence decoder. On AArch64 (where
    // NEON is part of the baseline) use the NEON kernel; otherwise keep the portable kernel.
    // Define IGUANA_DISABLE_DISPATCH to force the portable kernels (debugging / benchmarking).
#if defined(__aarch64__) && !defined(IGUANA_DISABLE_DISPATCH)
    g_Decompress = &decoder::decompress_neon;
#endif
}

void iguana::decoder::at_process_end() {}

std::uint8_t iguana::decoder::substream::fetch8(error_code& ec) noexcept {
    if (empty()) {
        ec = error_code::out_of_input_data;
        return 0;
    }
    ec = error_code::ok;
	return *m_cursor++;
}

std::uint8_t iguana::decoder::substream::fetch8() {
    if (empty()) {
        throw out_of_input_data_exception();
    }
	return *m_cursor++;
}

std::uint16_t iguana::decoder::substream::fetch16(error_code& ec) noexcept {
    if (remaining() < 2) {
        ec = error_code::out_of_input_data;    
        return 0;
    }
    ec = error_code::ok;
    const std::uint8_t a = m_cursor[0];
    const std::uint8_t b = m_cursor[1];
    m_cursor += 2;    
	return std::uint16_t(a) | (std::uint16_t(b) << 8);
}

std::uint16_t iguana::decoder::substream::fetch16() {
    if (remaining() < 2) {
        throw out_of_input_data_exception();
    }
    const std::uint8_t a = m_cursor[0];
    const std::uint8_t b = m_cursor[1];
    m_cursor += 2;    
	return std::uint16_t(a) | (std::uint16_t(b) << 8);
}

std::uint32_t iguana::decoder::substream::fetch24(error_code& ec) noexcept {
    if (remaining() < 3) {
        ec = error_code::out_of_input_data;    
        return 0;
    }
    ec = error_code::ok;
    const std::uint8_t a = m_cursor[0];
    const std::uint8_t b = m_cursor[1];
    const std::uint8_t c = m_cursor[2];
    m_cursor += 3;
	return std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16);
}

std::uint32_t iguana::decoder::substream::fetch24() {
    if (remaining() < 3) {
        throw out_of_input_data_exception();
    }
    const std::uint8_t a = m_cursor[0];
    const std::uint8_t b = m_cursor[1];
    const std::uint8_t c = m_cursor[2];
    m_cursor += 3;
	return std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16);
}

std::uint32_t iguana::decoder::substream::fetch_var_uint(error_code& ec) noexcept {
    const std::uint8_t a = fetch8(ec);

    if (ec != error_code::ok) {
        return 0;
    }

	if (a < 0xfe) {
		return std::uint32_t(a);
    } else if (a == 0xfe) {
        const std::uint32_t b = fetch16(ec);
        if (ec != error_code::ok) {
            return 0;
        }
		const std::uint32_t x0 = b & 0xff;
		const std::uint32_t x1 = (b >> 8);
		return (x1 * 254) + x0;
    } else {
        const std::uint32_t b = fetch24(ec);
        if (ec != error_code::ok) {
            return 0;
        }
		const std::uint32_t x0 = b & 0xff;
		const std::uint32_t x1 = (b >> 8) & 0xff;
		const std::uint32_t x2 = b >> 16;
		return (((x2 * 254) + x1) * 254) + x0;
    }
}

std::uint32_t iguana::decoder::substream::fetch_var_uint() {
    const std::uint8_t a = fetch8();

	if (a < 0xfe) {
		return std::uint32_t(a);
    } else if (a == 0xfe) {
        const std::uint32_t b = fetch16();
		const std::uint32_t x0 = b & 0xff;
		const std::uint32_t x1 = (b >> 8);
		return (x1 * 254) + x0;
    } else {
        const std::uint32_t b = fetch24();
		const std::uint32_t x0 = b & 0xff;
		const std::uint32_t x1 = (b >> 8) & 0xff;
		const std::uint32_t x2 = b >> 16;
		return (((x2 * 254) + x1) * 254) + x0;
    }
}

iguana::const_byte_span iguana::decoder::substream::fetch_sequence(std::size_t n, error_code& ec) noexcept {
    if (remaining() < n) {
        ec = error_code::out_of_input_data;    
        return {};
    }
    ec = error_code::ok;
    auto * const p = m_cursor;
    m_cursor += n;
    return { p, n };
}

iguana::const_byte_span iguana::decoder::substream::fetch_sequence(std::size_t n) {
    if (remaining() < n) {
        throw out_of_input_data_exception();
    }
    auto * const p = m_cursor;
    m_cursor += n;
    return { p, n };
}


iguana::decoder::entropy_buffer::entropy_buffer(std::size_t n) {
    const auto r = acquire_memory(n);
    m_data = r.first;
    m_cursor = 0;
    m_capacity = r.second;
}

iguana::decoder::entropy_buffer::~entropy_buffer() {
    release_memory(m_data);
}

iguana::decoder::entropy_buffer::entropy_buffer(entropy_buffer&& v) {
    m_data = std::exchange(v.m_data, nullptr);
    m_cursor = std::exchange(v.m_cursor, 0);
    m_capacity = std::exchange(v.m_capacity, 0);
}

iguana::decoder::entropy_buffer& iguana::decoder::entropy_buffer::operator =(entropy_buffer&& v) {
    if (this != &v) {
        std::uint8_t* const p = std::exchange(m_data, std::exchange(v.m_data, nullptr));
        m_cursor = std::exchange(v.m_cursor, 0);
        m_capacity = std::exchange(v.m_capacity, 0);
        release_memory(p);
    }
    return *this;
}

void iguana::decoder::entropy_buffer::reset(std::size_t n) {
    m_cursor = 0;

    if (n > capacity()) {
        release_memory(std::exchange(m_data, nullptr));
        const auto r = acquire_memory(n);
        m_data = r.first;
        m_capacity = r.second;
    }
}

std::pair<std::uint8_t*, std::size_t> iguana::decoder::entropy_buffer::acquire_memory(std::size_t n) {
    auto* const p = new std::uint8_t[n];
    return { p, n };
}

void iguana::decoder::entropy_buffer::release_memory(std::uint8_t* p) {
    if (p != nullptr) {
        delete[] p;   
    }
}
