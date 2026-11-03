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
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/call_error.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/response_value.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/problem.hpp>
#include <quoll/rest/reply.hpp>
#include <quoll/rest/unexpected_response.hpp>

#include <array>
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

template <class... Responses>
[[nodiscard]] constexpr bool has_status(type_list<Responses...>, [[maybe_unused]] http::status status) noexcept {
  return ((Responses::status == status) || ...);
}

// Why a response of a route's endpoint is not one reply<Endpoint> holds.
template <class Route>
[[nodiscard]] std::string unexpected_reason(http::status status) {
  auto const code = std::to_string(static_cast<int>(status));

  if (has_status(typename Route::responses{}, status)) {
    return "status " + code + " is declared by a with<> around the endpoint: reply<Endpoint> does not hold it";
  }

  return "undeclared status " + code;
}

// The declared response `Response`, if the response has its status. The response is moved into an error only.
template <class Endpoint, class Response>
void read_declared(http::response &response, std::optional<call_result<Endpoint>> &result) {
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
      auto reason = "invalid body: " + value.error().message;

      result.emplace(std::unexpected{unexpected_response{std::move(response), std::move(reason)}});
    }
  }
}

template <class Route>
[[nodiscard]] call_result<typename Route::endpoint> read_response(http::response response) {
  using endpoint = typename Route::endpoint;

  if constexpr (has_responses<endpoint>) {
    std::optional<call_result<endpoint>> result;

    [&]<class... Response>(type_list<Response...>) {
      (read_declared<endpoint, Response>(response, result), ...);
    }(own_responses<endpoint>{});

    if (result) {
      return std::move(*result);
    }
  } else if (response.status == http::status::no_content) {
    return {};
  }

  auto reason = unexpected_reason<Route>(response.status);

  return std::unexpected{unexpected_response{std::move(response), std::move(reason)}};
}

// The detail of a failed call: the problem's when the server sent one, else why the response is unexpected.
[[nodiscard]] inline std::string failure_detail(unexpected_response const &failure) {
  auto const content_type = failure.response.fields.find("Content-Type");

  if (content_type && http::detail::iequals(media_type(*content_type), problem_content_type)) {
    if (auto const decoded = lenient_json_codec{}.decode<problem>(failure.response.body); decoded && decoded->detail) {
      return *decoded->detail;
    }
  }

  return failure.reason;
}

[[noreturn]] inline void fail(unexpected_response failure) {
  auto detail = failure_detail(failure);

  throw call_error{std::move(failure), detail};
}

template <class Route>
async::task<call_result<typename Route::endpoint>> receive(async::task<http::response> sending) {
  co_return read_response<Route>(co_await std::move(sending));
}

// What call produces: the body of the first declared response, or nothing; any other outcome throws call_error.
template <class Route>
async::task<typename call_value<typename Route::endpoint>::type> receive_value(async::task<http::response> sending) {
  using endpoint = typename Route::endpoint;

  auto response = co_await std::move(sending);

  if constexpr (has_responses<endpoint>) {
    using first = typename front<own_responses<endpoint>>::type;

    if (response.status != first::status) {
      auto reason =
          has_status(own_responses<endpoint>{}, response.status)
              ? "declared response " + std::to_string(static_cast<int>(response.status)) + ": read it with try_call"
              : unexpected_reason<Route>(response.status);

      fail(unexpected_response{std::move(response), std::move(reason)});
    }

    auto result = read_response<Route>(std::move(response));

    if (!result) {
      fail(std::move(result.error()));
    }

    if constexpr (!std::is_void_v<typename first::type>) {
      co_return std::move(result->template get<first::status>());
    }
  } else {
    if (auto result = read_response<Route>(std::move(response)); !result) {
      fail(std::move(result.error()));
    }
  }
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_RESPONSE_HPP
