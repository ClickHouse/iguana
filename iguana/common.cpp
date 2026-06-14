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

#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "common.h"

//

void iguana::internal::unimplemented(const char* file_name, std::uint64_t line) {
    // NOTE (ClickHouse): the upstream port called std::abort() here, which would crash the
    // server when decompressing corrupted or hostile data that reaches an unimplemented code
    // path (e.g. an unsupported command byte in the bitstream). Throw instead so the error can
    // propagate and be reported as a decompression failure.
    throw std::runtime_error(
        std::string("iguana: invoked an unimplemented function ") + file_name + ":" + std::to_string(line));
}

bool iguana::internal::cpu_has_avx512() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    // Compiled without -mavx512*, so this detection runs safely on any x86-64 CPU.
    return __builtin_cpu_supports("avx512f")
        && __builtin_cpu_supports("avx512bw")
        && __builtin_cpu_supports("avx512vl")
        && __builtin_cpu_supports("avx512dq");
#else
    return false;
#endif
}
