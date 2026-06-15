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

#pragma once
#include "common.h"
#include <exception>
#include <string>

namespace iguana {

    enum class error_code : std::uint32_t {
        ok = 0,
        corrupted_bitstream,
        wrong_source_size,
        out_of_input_data,
        insufficient_target_capacity,
        unrecognized_command,
        out_of_memory
    };

    //

    const char* get_error_description(error_code ec);

    //

    class IGUANA_API exception : public std::exception {
        using super = std::exception;

    private:
        // NOTE (ClickHouse): the upstream port passed the message to std::exception(const char*),
        // which is a non-standard MSVC-only constructor and does not exist in libstdc++/libc++.
        // Store the message here and expose it through what() so the code is portable.
        std::string m_message;

    protected:
        exception() {}
        explicit exception(const char* msg) : m_message(msg ? msg : "") {}

    public:
        virtual ~exception();
        const char* what() const noexcept override { return m_message.empty() ? "iguana error" : m_message.c_str(); }
        virtual error_code get_error_code() const noexcept = 0;

    public:
        [[noreturn]] static void from_error(error_code ec);
    };

    //

    class IGUANA_API corrupted_bitstream_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit corrupted_bitstream_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~corrupted_bitstream_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };

    //

    class IGUANA_API wrong_source_size_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit wrong_source_size_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~wrong_source_size_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };

    //

    class IGUANA_API out_of_input_data_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit out_of_input_data_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~out_of_input_data_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };

    //

    class IGUANA_API insufficient_target_capacity_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit insufficient_target_capacity_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~insufficient_target_capacity_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };

    //

    class IGUANA_API unrecognized_command_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit unrecognized_command_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~unrecognized_command_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };

    //

    class IGUANA_API out_of_memory_exception : public exception {
        using super = exception;

    public:
        template <
            typename... TA
        > explicit out_of_memory_exception(TA&&... args)
          : super(std::forward<TA>(args)...) {}

        virtual ~out_of_memory_exception();

    public:
        virtual error_code get_error_code() const noexcept override final;
    };
}
