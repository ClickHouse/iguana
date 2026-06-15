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

// NOTE (ClickHouse): AVX-512 implementation of the 32-way interleaved rANS decoder. The decode
// kernel lives in the freestanding header ans32_avx512_core.h (shared with the standalone qemu
// validation harness); this file only adapts it to the codec's input_stream/output_stream types.
// Compiled only on x86-64 (see the CMake glue), with -mavx512f -mavx512bw -mavx512vl. Dispatched at
// process start by ans32::decoder::at_process_start when cpu_has_avx512() is true.

#include "ans32.h"
#include "ans32_avx512_core.h"

void iguana::ans32::decoder::decompress_avx512(context& ctx)
{
    const std::size_t result_size = ctx.result_size;
    std::uint8_t * const out = ctx.dst.claim(result_size);
    if (iguana_avx512::ans32_decode(ctx.src.data(), ctx.src.size(), ctx.tab, result_size, out))
        ctx.ec = error_code::ok;
    else
        ctx.ec = error_code::corrupted_bitstream;
}
