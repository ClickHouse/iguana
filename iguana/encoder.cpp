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

#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <numeric>
#include <vector>
#include <algorithm>
#include "encoder.h"
#include "bitops.h"
#include "error.h"
#include "ans32.h"
#include "ans1.h"
#include "ans_nibble.h"
#include "utils.h"

//

namespace iguana {
    const internal::initializer<encoder> encoder::g_Initializer;

    //

    template <typename T_ENCODER> command decoding_command = utils::invalid<T_ENCODER>::value; 
    template <> command decoding_command<ans32::encoder> = command::decode_ans32; 
    template <> command decoding_command<ans1::encoder> = command::decode_ans1; 
    template <> command decoding_command<ans_nibble::encoder> = command::decode_ans_nibble; 
}

//

iguana::encoding iguana::encoding_from_string(const char* name) {
    if (std::strcmp(name, "iguana") == 0) {
        return encoding::iguana;
    }

    if (std::strcmp(name, "raw") == 0) {
        return encoding::raw;
    }

    throw std::invalid_argument(std::string("unrecognized encoding '") + name + "'");
}

const char* iguana::to_string(encoding e) {
    switch(e) {
        case encoding::iguana:
            return "iguana";

        case encoding::raw:
            return "raw";

        default:
            throw std::invalid_argument("unrecognized encoding value");        
    }
}

//

iguana::encoder::encoder() {}

iguana::encoder::~encoder() {}

void iguana::encoder::at_process_start() {}

void iguana::encoder::at_process_end() {}

void iguana::encoder::encode_entropy_raw(output_stream& dst, const part& p) {
	append_control_command(command::copy_raw);
    append_control_var_uint(p.m_size);
    dst.append(p.m_data, p.m_size);
}

template <
    typename T_ENCODER
> void iguana::encoder::encode_entropy(output_stream& dst, const part& p) {
    T_ENCODER{}.encode(m_entropy_data, p.m_data, p.m_size);

    const auto src_len = p.m_size;
    const auto entropy_len = m_entropy_data.size();

    if (const auto ratio = double(entropy_len) / double(src_len); ratio >= p.m_rejection_threshold) {
        encode_entropy_raw(dst, p);
    } else {
        append_control_command(decoding_command<T_ENCODER>);
        append_control_var_uint(src_len);
        append_control_var_uint(entropy_len);
        dst.append(m_entropy_data);
    }

    m_entropy_data.clear();
}

// ---------------------------------------------------------------------------------------------
// NOTE (ClickHouse): the Iguana structural (LZ) compressor below was ported from the Go reference
// implementation (github.com/SnellerInc/sneller, ion/zion/iguana/encoder.go). The upstream C++
// port left encode_iguana unimplemented. The match finder, token format and substream layout match
// the Go reference and the (already implemented) C++ portable sequence decoder.
// ---------------------------------------------------------------------------------------------
namespace iguana {
namespace {

constexpr std::int32_t  IG_LITERAL_LEN_BITS    = 3;
constexpr std::uint32_t IG_MM_LONG_OFFSETS     = 16;
constexpr std::uint32_t IG_MAX_SHORT_LIT_LEN   = 7;
constexpr std::uint32_t IG_MAX_SHORT_MATCH_LEN = 15;
constexpr std::uint32_t IG_LAST_LONG_OFFSET    = 31;
constexpr std::int32_t  IG_MIN_OFFSET          = 32;   // == iguanaChunkSize
constexpr std::int32_t  IG_MIN_LENGTH          = 32;
constexpr std::uint32_t IG_MAX_UINT16          = (1u << 16) - 1;
constexpr std::uint32_t IG_VARUINT_1B          = 254;
constexpr std::uint32_t IG_VARUINT_3B          = 254u * 254u;
constexpr std::uint32_t IG_CHAIN_BITS          = 17;
constexpr std::uint32_t IG_HASH_BYTES          = 5;
constexpr std::size_t   IG_HIST_SIZE           = 4;
constexpr std::size_t   IG_STREAM_COUNT        = 6;

inline void append_var_uint(std::vector<std::uint8_t>& s, std::uint32_t v) {
    if (v < IG_VARUINT_1B) {
        s.push_back(static_cast<std::uint8_t>(v));
    } else if (v < IG_VARUINT_3B) {
        s.push_back(254);
        s.push_back(static_cast<std::uint8_t>(v % 254));
        s.push_back(static_cast<std::uint8_t>(v / 254));
    } else {
        // v < 254*254*254 by design (substream offsets/lengths are bounded by the block size)
        const std::uint32_t t = v / 254;
        s.push_back(255);
        s.push_back(static_cast<std::uint8_t>(v % 254));
        s.push_back(static_cast<std::uint8_t>(t % 254));
        s.push_back(static_cast<std::uint8_t>(t / 254));
    }
}

inline void append_u16(std::vector<std::uint8_t>& s, std::uint32_t v) {
    s.push_back(static_cast<std::uint8_t>(v));
    s.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline void append_u24(std::vector<std::uint8_t>& s, std::uint32_t v) {
    s.push_back(static_cast<std::uint8_t>(v));
    s.push_back(static_cast<std::uint8_t>(v >> 8));
    s.push_back(static_cast<std::uint8_t>(v >> 16));
}

inline bool is_legal(std::int32_t offs, std::int32_t length) {
    return offs <= static_cast<std::int32_t>(IG_MAX_UINT16) || length > static_cast<std::int32_t>(IG_MAX_SHORT_MATCH_LEN);
}

// longest common prefix of src[lo:] and src[hi:], with lo < hi
inline std::int32_t lcp(const std::uint8_t* src, std::size_t src_len, std::int32_t lo, std::int32_t hi) {
    std::int32_t matched = 0;
    while (static_cast<std::ptrdiff_t>(src_len) - static_cast<std::ptrdiff_t>(hi + matched) >= 8) {
        const std::uint64_t a = utils::read_little_endian<std::uint64_t>(src + lo + matched);
        const std::uint64_t b = utils::read_little_endian<std::uint64_t>(src + hi + matched);
        if (const std::uint64_t delta = a ^ b; delta != 0)
            return matched + static_cast<std::int32_t>(__builtin_ctzll(delta) / 8);
        matched += 8;
    }
    while (static_cast<std::ptrdiff_t>(src_len) - static_cast<std::ptrdiff_t>(hi + matched) > 0
           && src[lo + matched] == src[hi + matched]) {
        ++matched;
    }
    return matched;
}

struct match_result { std::int32_t target = 0, pos = 0, len = 0; };

// produce a legal (targetpos, matchpos, matchlen), extending the match backwards, or {0,0,0}
inline match_result do_match(const std::uint8_t* src, std::size_t src_len, std::int32_t minto, std::int32_t from, std::int32_t to) {
    std::int32_t targetpos = to;
    std::int32_t matchpos = from;
    std::int32_t matchlen = lcp(src, src_len, matchpos, targetpos);
    while (matchpos > 0 && src[matchpos - 1] == src[targetpos - 1] && targetpos > minto) {
        --matchpos;
        --targetpos;
        ++matchlen;
    }
    if (!is_legal(targetpos - matchpos, matchlen))
        return {};
    return {targetpos, matchpos, matchlen};
}

// Hash-chain match table (1 << chainbits entries, each keeping the last histsize positions).
struct match_table {
    struct entry { std::int32_t history[IG_HIST_SIZE]; };
    std::vector<entry> entries;

    match_table() : entries(std::size_t(1) << IG_CHAIN_BITS) {}

    void reset() { std::fill(entries.begin(), entries.end(), entry{}); }

    static std::uint32_t hash(const std::uint8_t* seq) {
        std::uint64_t u = utils::read_little_endian<std::uint64_t>(seq);
        u = (u << 24) * 889523592379ULL;   // IG_HASH_BYTES == 5
        return static_cast<std::uint32_t>(u >> (64 - IG_CHAIN_BITS));
    }

    void insert(const std::uint8_t* src, std::int32_t pos) {
        entry& e = entries[hash(src + pos)];
        e.history[3] = e.history[2];
        e.history[2] = e.history[1];
        e.history[1] = e.history[0];
        e.history[0] = pos;
    }

    match_result best_match(const std::uint8_t* src, std::size_t src_len, std::int32_t litmin, std::int32_t pos) const {
        const entry& e = entries[hash(src + pos)];
        match_result best = do_match(src, src_len, litmin, e.history[0], pos);
        for (std::size_t i = 1; i < IG_HIST_SIZE; ++i) {
            const std::int32_t cpos = e.history[i];
            if (cpos == 0)
                break; // empty slot
            if (const match_result alt = do_match(src, src_len, litmin, cpos, pos); alt.len > best.len)
                best = alt;
        }
        return best;
    }
};

struct lz_compressor {
    const std::uint8_t* src = nullptr;
    std::size_t src_len = 0;
    std::vector<std::uint8_t> tokens, offsets16, offsets24, var_lit_len, var_match_len, literals;
    std::uint32_t last_encoded_offset = 0;
    match_table table;

    match_result best_match_at(std::int32_t litpos, std::int32_t pos) {
        match_result r;
        r.target = pos;
        // first try the last encoded offset (cheap "repeat offset")
        if (const std::int32_t p = pos - static_cast<std::int32_t>(last_encoded_offset); p < pos) {
            r.pos = p;
            r.len = lcp(src, src_len, p, pos);
        }
        // then the hash table; it must beat the repeat offset by 2 bytes or more
        if (const match_result hm = table.best_match(src, src_len, litpos, pos); hm.len - r.len > 1)
            r = hm;

        // if the end of the match is within one register distance of the end of the buffer, make
        // sure the decoder's final (up to 32-byte) copy of this match stays in bounds
        if (r.target + r.len > static_cast<std::int32_t>(src_len) - IG_MIN_OFFSET) {
            if (r.target - r.pos >= IG_MIN_OFFSET) {
                constexpr std::int32_t lomask = IG_MIN_OFFSET - 1;
                if (r.target + ((r.len + lomask) & ~lomask) > static_cast<std::int32_t>(src_len))
                    r.len &= ~lomask;
            } else {
                const std::int32_t movsize = r.target - r.pos;
                const std::int32_t tailpos = r.len - (r.len % movsize);
                const std::int32_t end = static_cast<std::int32_t>(src_len);
                if (r.target + tailpos + IG_MIN_OFFSET > end) {
                    const std::int32_t safedist = (end - IG_MIN_OFFSET) - r.target;
                    r.len = (safedist / movsize) * movsize;
                }
            }
        }
        return r;
    }

    void emit(const std::uint8_t* lit, std::size_t lit_len, std::uint32_t offs, std::uint32_t match_len) {
        literals.insert(literals.end(), lit, lit + lit_len);
        if (offs == last_encoded_offset || offs <= IG_MAX_UINT16) {
            std::uint8_t token = 0x80;
            if (offs != last_encoded_offset) {
                token = 0x00;
                append_u16(offsets16, offs);
            }
            if (lit_len < IG_MAX_SHORT_LIT_LEN) {
                token |= static_cast<std::uint8_t>(lit_len);
            } else {
                token |= static_cast<std::uint8_t>(IG_MAX_SHORT_LIT_LEN);
                append_var_uint(var_lit_len, static_cast<std::uint32_t>(lit_len) - IG_MAX_SHORT_LIT_LEN);
            }
            if (match_len < IG_MAX_SHORT_MATCH_LEN) {
                token |= static_cast<std::uint8_t>(match_len << IG_LITERAL_LEN_BITS);
            } else {
                token |= static_cast<std::uint8_t>(IG_MAX_SHORT_MATCH_LEN << IG_LITERAL_LEN_BITS);
                append_var_uint(var_match_len, match_len - IG_MAX_SHORT_MATCH_LEN);
            }
            tokens.push_back(token);
        } else {
            // offsets >= 2^16 cannot carry literals, so flush any pending literals first
            if (lit_len > 0) {
                std::uint8_t token = 0x80; // retain the offset, no match part
                if (lit_len < IG_MAX_SHORT_LIT_LEN) {
                    token |= static_cast<std::uint8_t>(lit_len);
                } else {
                    token |= static_cast<std::uint8_t>(IG_MAX_SHORT_LIT_LEN);
                    append_var_uint(var_lit_len, static_cast<std::uint32_t>(lit_len) - IG_MAX_SHORT_LIT_LEN);
                }
                tokens.push_back(token);
            }
            // a long-offset match always has match_len > IG_MAX_SHORT_MATCH_LEN (enforced by is_legal)
            append_u24(offsets24, offs);
            std::uint8_t token;
            if (match_len < (IG_LAST_LONG_OFFSET + IG_MM_LONG_OFFSETS)) {
                token = static_cast<std::uint8_t>(match_len - IG_MM_LONG_OFFSETS);
            } else {
                token = 0x1f;
                append_var_uint(var_match_len, match_len - (IG_LAST_LONG_OFFSET + IG_MM_LONG_OFFSETS));
            }
            tokens.push_back(token);
        }
        last_encoded_offset = offs;
    }

    void compress() {
        constexpr std::int32_t skip_step = 2;
        table.reset();
        const std::int32_t last = static_cast<std::int32_t>(src_len) - IG_MIN_OFFSET; // last allowed match position
        last_encoded_offset = 0;
        std::int32_t pos = 5;     // current match search position
        std::int32_t litpos = 0;
        table.insert(src, 0);

        // search for matches up to and including the last allowed match position
        while (pos <= last) {
            match_result m = best_match_at(litpos, pos);
            // see if the very next byte produces a longer match; if so, use it rather than breaking
            // up a larger potential match
            if (pos < last) {
                if (const match_result m1 = best_match_at(litpos, pos + 1); m1.len > m.len)
                    m = m1;
            }
            if (m.len >= 4) {
                emit(src + litpos, static_cast<std::size_t>(m.target - litpos),
                     static_cast<std::uint32_t>(m.target - m.pos), static_cast<std::uint32_t>(m.len));
                // insert the positions covered by the match (skipping by skip_step)
                for (std::int32_t i = m.target; i < (m.target + m.len) && i < last; i += skip_step)
                    table.insert(src, i);
                pos = m.target + m.len; // advance past the match
                litpos = pos;
            } else {
                table.insert(src, pos);
                pos += skip_step;
            }
        }
        // flush remaining literals
        literals.insert(literals.end(), src + litpos, src + src_len);
    }
};

// Entropy-compress each substream with a single reused encoder of type ENC.
template <typename ENC>
void compress_streams(double threshold, std::uint8_t mode_value,
                      std::vector<std::uint8_t>* const* ustreams,
                      output_stream* cstreams,
                      std::uint64_t& hdr, std::size_t& total_size) {
    ENC enc;
    for (std::size_t i = 0; i < IG_STREAM_COUNT; ++i) {
        const std::vector<std::uint8_t>& u = *ustreams[i];
        if (u.empty())
            continue; // empty stream: store verbatim (matches the Go reference's +Inf ratio)
        cstreams[i].clear();
        enc.encode(cstreams[i], u.data(), u.size());
        if (double(cstreams[i].size()) / double(u.size()) < threshold) {
            hdr |= (static_cast<std::uint64_t>(mode_value) << (i * 4));
            total_size -= u.size();
            total_size += cstreams[i].size();
        } else {
            cstreams[i].clear();
        }
    }
}

} // anonymous namespace
} // namespace iguana

void iguana::encoder::encode_iguana(output_stream& dst, const part& p) {
    // We need enough bytes to actually produce a hash-chain match within the preceding minLength
    // bytes; otherwise store the block verbatim.
    if (p.m_size < static_cast<std::size_t>(IG_MIN_LENGTH + IG_HASH_BYTES)) {
        encode_entropy_raw(dst, p);
        return;
    }

    lz_compressor lz;
    lz.src = p.m_data;
    lz.src_len = p.m_size;
    lz.compress();

    std::vector<std::uint8_t>* const ustreams[IG_STREAM_COUNT] = {
        &lz.tokens, &lz.offsets16, &lz.offsets24, &lz.var_lit_len, &lz.var_match_len, &lz.literals
    };

    std::uint64_t hdr = 0;
    output_stream cstreams[IG_STREAM_COUNT];
    std::size_t total_size = 0;
    for (std::size_t i = 0; i < IG_STREAM_COUNT; ++i)
        total_size += ustreams[i]->size();

    if (p.m_rejection_threshold > 0.0) {
        switch (p.m_entropy_mode) {
        case entropy_mode::ans32:
            compress_streams<ans32::encoder>(p.m_rejection_threshold, static_cast<std::uint8_t>(entropy_mode::ans32), ustreams, cstreams, hdr, total_size);
            break;
        case entropy_mode::ans1:
            compress_streams<ans1::encoder>(p.m_rejection_threshold, static_cast<std::uint8_t>(entropy_mode::ans1), ustreams, cstreams, hdr, total_size);
            break;
        case entropy_mode::ans_nibble:
            compress_streams<ans_nibble::encoder>(p.m_rejection_threshold, static_cast<std::uint8_t>(entropy_mode::ans_nibble), ustreams, cstreams, hdr, total_size);
            break;
        case entropy_mode::none:
            break;
        default:
            throw std::invalid_argument("unrecognized entropy mode");
        }
    }

    // If nothing was gained, fall back to a verbatim copy (assume each stream consumes at least one
    // varint byte and the header includes one extra control byte).
    if (total_size + IG_STREAM_COUNT + 1 >= p.m_size) {
        encode_entropy_raw(dst, p);
        return;
    }

    append_control_command(command::decode_iguana);
    append_control_var_uint(hdr);

    // Append the uncompressed streams' lengths.
    for (std::size_t i = 0; i < IG_STREAM_COUNT; ++i)
        append_control_var_uint(ustreams[i]->size());

    // Append streams' data (verbatim or entropy-coded) and, for the latter, the compressed lengths.
    for (std::size_t i = 0; i < IG_STREAM_COUNT; ++i) {
        if (static_cast<entropy_mode>((hdr >> (i * 4)) & 0x0f) == entropy_mode::none) {
            dst.append(ustreams[i]->data(), ustreams[i]->size());
        } else {
            append_control_var_uint(cstreams[i].size());
            dst.append(cstreams[i].data(), cstreams[i].size());
        }
    }
}

void iguana::encoder::encode(output_stream& dst, const std::uint8_t* p, std::size_t n) {
    // NOTE (ClickHouse): the upstream port built the part without setting m_data/m_size (so it
    // compressed nothing) and selected encoding::iguana while encode_iguana was unimplemented.
    // Now that the structural compressor is implemented, wire the input through correctly.
    const part prt = {
        .m_data = p,
        .m_size = n,
        .m_entropy_mode = iguana::entropy_mode::ans32,
        .m_encoding = iguana::encoding::iguana,
        .m_rejection_threshold = default_rejection_threshold
    };
    encode(dst, prt);
}

void iguana::encoder::encode(output_stream& dst, const part& p) {
    m_last_command_offset = -1;
    append_control_var_uint(p.m_size);
    encode_part(dst, p);

	// Append the control bytes in reverse order
    dst.append_reverse(m_control.data(), m_control.size());
    m_control.clear();
}

void iguana::encoder::encode(output_stream& dst, const part* first, const part* last) {
    m_last_command_offset = -1;

    // Compute the total input size
    {   const auto total_input_size = std::accumulate(
            first,
            last,
            std::uint64_t(0),
            [](std::uint64_t lhs, const part& rhs) {
                return lhs + rhs.m_size;
            }
        );
        append_control_var_uint(total_input_size);
    }

    for(const auto* i = first; i != last; ++i) {
        encode_part(dst, *i);
    }

	// Append the control bytes in reverse order
    dst.append_reverse(m_control.data(), m_control.size());
    m_control.clear();
}

void iguana::encoder::encode_part(output_stream& dst, const part& p) {
    if (p.m_size == 0) [[unlikely]] { 
        return;
    }

    switch(p.m_encoding) {
    case encoding::raw:
        switch(p.m_entropy_mode) {
        case entropy_mode::none:
            encode_entropy_raw(dst, p);
            break;

        case entropy_mode::ans32:
            encode_entropy<ans32::encoder>(dst, p);
            break;

        case entropy_mode::ans1:
            encode_entropy<ans1::encoder>(dst, p);
            break;

        case entropy_mode::ans_nibble:
            encode_entropy<ans_nibble::encoder>(dst, p);
            break;

        default:
            throw std::invalid_argument(std::string("unrecognized entropy mode '") + to_string(p.m_entropy_mode) + "'");              
        }
        break;

    case encoding::iguana:
        encode_iguana(dst, p);
        break;

    default:
        throw std::invalid_argument(std::string("unrecognized encoding '") + to_string(p.m_encoding) + "'");
    }
}

void iguana::encoder::append_control_var_uint(std::uint64_t v) {
    const auto cnt = (bit::length(v) / 7) + 1;

	for(auto i = static_cast<int>(cnt) - 1; i >= 0; --i) {
		auto x = static_cast<std::uint8_t>((v >> (i*7)) & 0x7f);
		if (i == 0) {
			x |= 0x80;
		}
        m_control.push_back(x);
	}
}

void iguana::encoder::append_control_command(command cmd) {
	if (m_last_command_offset >= 0) {
		m_control[m_last_command_offset] &= command_mask;
	}

	m_last_command_offset = m_control.size();
    m_control.push_back(static_cast<std::uint8_t>(cmd) | last_command_marker);
}
