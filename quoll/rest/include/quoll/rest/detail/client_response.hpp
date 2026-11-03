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
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/response_value.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/http_error.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/problem.hpp>
#include <quoll/rest/reply.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

template <class Endpoint>
inline constexpr bool has_responses = own_responses<Endpoint>::size > 0;

// What try_call produces: one of the endpoint's responses, or the 204 of an endpoint without returns<>.
template <class Endpoint>
using call_result = std::conditional_t<has_responses<Endpoint>, std::expected<reply<Endpoint>, unexpected_response>,
                                       std::expected<void, unexpected_response>>;

template <class Endpoint, bool = has_responses<Endpoint>>
struct call_value {
  using type = void;
};

template <class Endpoint>
struct call_value<Endpoint, true> {
  using type = typename front<own_responses<Endpoint>>::type::type;
};

[[nodiscard]] inline unexpected_response unexpected(http::response response, std::string reason) {
  return {std::move(response), std::move(reason)};
}

// The declared response `Response`, if the response has its status.
template <class Endpoint, class Response>
void read_declared(http::response const &response, std::optional<call_result<Endpoint>> &result) {
  if (result || response.status != Response::status) {
    return;
  }

  if constexpr (std::is_void_v<typename Response::type>) {
    result.emplace(reply<Endpoint>{respond<Response::status>()});
  } else {
    constexpr typename Response::codec codec;

    if (auto value = codec.template decode<typename Response::type>(response.body)) {
      result.emplace(reply<Endpoint>{respond<Response::status>(std::move(*value))});
    } else {
      result.emplace(std::unexpected{unexpected(response, "invalid body: " + value.error().message)});
    }
  }
}

template <class Endpoint>
[[nodiscard]] call_result<Endpoint> read_response(http::response response) {
  auto const undeclared = [&] {
    auto reason = "undeclared status " + std::to_string(static_cast<int>(response.status));

    return std::unexpected{unexpected(std::move(response), std::move(reason))};
  };

  if constexpr (has_responses<Endpoint>) {
    std::optional<call_result<Endpoint>> result;

    [&]<class... Response>(type_list<Response...>) {
      (read_declared<Endpoint, Response>(response, result), ...);
    }(own_responses<Endpoint>{});

    if (result) {
      return std::move(*result);
    }

    return undeclared();
  } else {
    if (response.status == http::status::no_content) {
      return {};
    }

    return undeclared();
  }
}

// The detail of a failed call: the problem's when the server sent one, else why the response is unexpected.
[[nodiscard]] inline std::string failure_detail(unexpected_response const &failure) {
  auto const content_type = failure.response.fields.find("Content-Type");

  if (content_type && media_type(*content_type) == problem_content_type) {
    if (auto const decoded = lenient_json_codec{}.decode<problem>(failure.response.body); decoded && decoded->detail) {
      return *decoded->detail;
    }
  }

  return failure.reason;
}

template <class Endpoint>
async::task<call_result<Endpoint>> receive(async::task<http::response> sending) {
  co_return read_response<Endpoint>(co_await std::move(sending));
}

// What call produces: the body of the first declared response, or nothing; any other outcome throws http_error.
template <class Endpoint>
async::task<typename call_value<Endpoint>::type> receive_value(async::task<http::response> sending) {
  auto result = read_response<Endpoint>(co_await std::move(sending));

  if (!result) {
    throw http_error{result.error().response.status, failure_detail(result.error())};
  }

  if constexpr (has_responses<Endpoint>) {
    using first = typename front<own_responses<Endpoint>>::type;

    if (!result->template holds<first::status>()) {
      throw http_error{result->status(), "declared response " + std::to_string(static_cast<int>(result->status())) +
                                             ": use try_call to read it"};
    }

    if constexpr (!std::is_void_v<typename first::type>) {
      co_return std::move(result->template get<first::status>());
    }
  }
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_RESPONSE_HPP
