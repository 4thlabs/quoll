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

#ifndef QUOLL_REST_DETAIL_CLIENT_RESPONSE_HPP
#define QUOLL_REST_DETAIL_CLIENT_RESPONSE_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/refusal.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

template <class Endpoint>
inline constexpr bool has_result = own_responses<Endpoint>::size > 0;

// What a route answers on success: its result's value, or nothing for a void one.
template <class Endpoint, bool = has_result<Endpoint>>
struct call_value {
  using type = void;
};

template <class Endpoint>
struct call_value<Endpoint, true> {
  using type = typename front<own_responses<Endpoint>>::type::type;
};

// What try_call produces: the result's value, or the http::http_error received.
template <class Endpoint>
using call_result = std::expected<typename call_value<Endpoint>::type, http::http_error>;

// Whether `status` is a success, 2xx: a route's own (200, 201, 202, 204 for a void one) or another a server answers for
// it, as a third-party API's 200 to a HEAD or to a DELETE with nothing to say. A result with a body takes any but 204,
// 205 and 206.
[[nodiscard]] constexpr bool is_success(http::status status) noexcept {
  auto const code = static_cast<int>(status);

  return code >= 200 && code < 300;
}

// The value of `response`, or why it is none: a status that is no success, or a body that does not decode. A void
// result reads no body.
template <class Route>
[[nodiscard]] std::expected<typename call_value<typename Route::endpoint>::type, refusal>
read_response(http::response response) {
  using value_type = typename call_value<typename Route::endpoint>::type;

  if (!is_success(response.status)) {
    auto reason = "status " + std::to_string(static_cast<int>(response.status));

    return std::unexpected{refusal{std::move(response), std::move(reason)}};
  }

  if constexpr (std::is_void_v<value_type>) {
    return {};
  } else {
    // A result is a whole body: none comes with a 204 or a 205, and a 206 is a part of one, which no route asks for.
    if (auto const code = static_cast<int>(response.status); code >= 204 && code <= 206) {
      auto reason = "status " + std::to_string(code) + " with no whole body";

      return std::unexpected{refusal{std::move(response), std::move(reason)}};
    }

    using result = typename front<own_responses<typename Route::endpoint>>::type;

    typename result::codec const codec{};

    if (auto value = codec.template decode<value_type>(response.body)) {
      return std::move(*value);
    } else {
      auto reason = "invalid body: " + value.error().message;

      return std::unexpected{refusal{std::move(response), std::move(reason)}};
    }
  }
}

template <class Route>
async::task<call_result<typename Route::endpoint>> receive(async::task<http::response> sending) {
  auto read = read_response<Route>(co_await std::move(sending));

  if (!read) {
    co_return std::unexpected{received_error(std::move(read.error()))};
  }

  if constexpr (std::is_void_v<typename call_value<typename Route::endpoint>::type>) {
    co_return call_result<typename Route::endpoint>{};
  } else {
    co_return std::move(*read);
  }
}

// What call produces: the result's value, or nothing; any other outcome throws an http::http_error received.
template <class Route>
async::task<typename call_value<typename Route::endpoint>::type> receive_value(async::task<http::response> sending) {
  auto read = read_response<Route>(co_await std::move(sending));

  if (!read) {
    fail(std::move(read.error()));
  }

  if constexpr (!std::is_void_v<typename call_value<typename Route::endpoint>::type>) {
    co_return std::move(*read);
  }
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_RESPONSE_HPP
