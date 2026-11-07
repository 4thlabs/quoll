//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef QUOLL_HTTP_DETAIL_BODY_STREAM_TABLE_HPP
#define QUOLL_HTTP_DETAIL_BODY_STREAM_TABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::http::detail {

// Whether a sink has a member named `head`, whatever it is: overloaded, a template or private. Naming it through what
// also inherits one is then ambiguous.
struct another_head {
  int head;
};

template <class Sink>
struct both_heads : Sink, another_head {};

template <class Sink>
inline constexpr bool has_no_head = std::is_final_v<Sink> || requires { &both_heads<Sink>::head; };

// What a body_stream knows of what it writes through.
struct body_stream_table {
  async::task<> (*write)(void *, std::string_view);
  void (*head)(void *, http::status, http::fields);
  async::detail::type_id type;
};

template <class Sink>
inline constexpr body_stream_table body_stream_table_of{
    .write = [](void *erased, std::string_view piece) { return static_cast<Sink *>(erased)->write(piece); },
    .head =
        [](void *erased, http::status status, http::fields fields) {
          if constexpr (requires(Sink &target) { target.head(status, std::move(fields)); }) {
            static_cast<Sink *>(erased)->head(status, std::move(fields));
          } else {
            // One with another signature would lose the status and fields of what it wraps, with no one told.
            static_assert(has_no_head<Sink>,
                          "quoll::http::body_stream: a sink's head() takes an http::status and an http::fields");
          }
        },
    .type = async::detail::type_id_of<Sink>(),
};

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_BODY_STREAM_TABLE_HPP
