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

#ifndef QUOLL_REST_DETAIL_SERVE_HPP
#define QUOLL_REST_DETAIL_SERVE_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/credential.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/query_string.hpp>
#include <quoll/rest/detail/response_value.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/http_error.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/problem.hpp>
#include <quoll/rest/reply.hpp>
#include <quoll/rest/security.hpp>
#include <quoll/rest/validators.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

// What mount() keeps for the routes it adds: the validators and the services.
template <class Validators, class... Services>
struct mounted {
  Validators validators;
  std::tuple<Services...> services;
};

template <class T>
struct task_value {
  static constexpr bool is_task = false;
};

template <class T>
struct task_value<async::task<T>> {
  static constexpr bool is_task = true;
  using type = T;
};

// An error the server answers itself, as an RFC 9457 problem; `detail` is sent as it is.
[[nodiscard]] inline http::response problem_response(http::status code, std::optional<std::string> detail) {
  auto const reason = http::reason_phrase(code);
  problem const body{
      .type = std::nullopt,
      .title = reason.empty() ? std::nullopt : std::optional<std::string>{reason},
      .status = static_cast<int>(code),
      .detail = std::move(detail),
      .instance = std::nullopt,
  };
  http::response response;

  response.status = code;
  response.body = json_codec{}.encode(body);
  response.fields.add("Content-Type", problem_content_type);
  return response;
}

// The credential a request carries for a scheme, if any.
template <fixed_string Header>
[[nodiscard]] std::optional<std::string> find_credential(std::type_identity<api_key<Header>>,
                                                         request_input const &input) {
  auto const value = input.request.fields.find(Header.view());

  // An empty key is no key, as an empty bearer token is no token.
  return value && !value->empty() ? std::optional<std::string>{*value} : std::nullopt;
}

template <fixed_string Param>
[[nodiscard]] std::optional<std::string> find_credential(std::type_identity<api_key_query<Param>>,
                                                         request_input const &input) {
  auto const texts = query_texts(input.query, Param.view());

  return texts.empty() || texts.front().empty() ? std::nullopt : std::optional<std::string>{texts.front()};
}

[[nodiscard]] inline std::optional<std::string> find_credential(std::type_identity<bearer_auth>,
                                                                request_input const &input) {
  constexpr std::string_view scheme = "Bearer ";
  auto const value = input.request.fields.find("Authorization");

  if (!value || value->size() <= scheme.size() || !http::detail::iequals(value->substr(0, scheme.size()), scheme)) {
    return std::nullopt;
  }

  auto const token = http::detail::trim(value->substr(scheme.size()));

  return token.empty() ? std::nullopt : std::optional<std::string>{token};
}

// The challenge of a 401, which RFC 9110 requires: `Bearer`, or `ApiKey` with where the key goes (API keys have no
// registered scheme; the name tells the client what to send).
[[nodiscard]] inline std::string challenge(std::type_identity<bearer_auth>) { return "Bearer"; }

template <fixed_string Header>
[[nodiscard]] std::string challenge(std::type_identity<api_key<Header>>) {
  return "ApiKey header=\"" + std::string{Header.view()} + "\"";
}

template <fixed_string Param>
[[nodiscard]] std::string challenge(std::type_identity<api_key_query<Param>>) {
  return "ApiKey query=\"" + std::string{Param.view()} + "\"";
}

template <class Scheme>
[[nodiscard]] http::response unauthorized(std::string detail) {
  auto response = problem_response(http::status::unauthorized, std::move(detail));

  response.fields.add("WWW-Authenticate", challenge(std::type_identity<Scheme>{}));
  return response;
}

// The validator taking the credentials of `Scheme`: the first one invocable with them.
template <class Scheme, class... Validators>
[[nodiscard]] consteval std::size_t validator_index(type_list<Validators...>) {
  return first_true(
      std::array<bool, sizeof...(Validators)>{std::is_invocable_v<Validators &, credential<Scheme> const &>...});
}

template <class Scheme, class... Validators>
async::task<bool> accepts(validator_set<Validators...> &set, credential<Scheme> const value) {
  constexpr auto index = validator_index<Scheme>(type_list<Validators...>{});

  static_assert(index < sizeof...(Validators),
                "quoll::rest::mount: no validator takes the credential<Scheme> of a scheme the api uses");

  auto &validator = std::get<index>(set.validators);

  if constexpr (task_value<std::invoke_result_t<decltype(validator), credential<Scheme> const &>>::is_task) {
    co_return co_await std::invoke(validator, value);
  } else {
    co_return static_cast<bool>(std::invoke(validator, value));
  }
}

// Every scheme of the route must be satisfied; the 401 of the first one that is not, or nothing.
template <class Validators>
async::task<std::optional<http::response>> check_security(type_list<>, Validators &, request_input const &) {
  co_return std::nullopt;
}

template <class Scheme, class... Rest, class Validators>
async::task<std::optional<http::response>> check_security(type_list<Scheme, Rest...>, Validators &validators,
                                                          request_input const &input) {
  auto value = find_credential(std::type_identity<Scheme>{}, input);

  if (!value) {
    co_return unauthorized<Scheme>("missing credentials");
  }

  if (!co_await accepts(validators, credential<Scheme>{std::move(*value)})) {
    co_return unauthorized<Scheme>("rejected credentials");
  }

  co_return co_await check_security(type_list<Rest...>{}, validators, input);
}

// A declared response, as an HTTP response: with its body, or without for a void one.
template <class Response>
[[nodiscard]] http::response make_response() {
  http::response response;

  response.status = Response::status;
  return response;
}

template <class Response, class T>
[[nodiscard]] http::response make_response(T const &value) {
  constexpr typename Response::codec codec;
  auto response = make_response<Response>();

  response.body = codec.encode(value);
  response.fields.add("Content-Type", codec.content_type);
  return response;
}

// What a service returned: a reply<Endpoint>, or the body of the endpoint's first response.
template <class Endpoint, class T>
[[nodiscard]] http::response encode_result(T const &value) {
  if constexpr (std::same_as<T, reply<Endpoint>>) {
    return value.visit([](auto const &held) {
      using response = typename std::decay_t<decltype(held)>::response;

      if constexpr (std::is_void_v<typename response::type>) {
        return make_response<response>();
      } else {
        return make_response<response>(held.value);
      }
    });
  } else {
    using responses = own_responses<Endpoint>;

    static_assert(responses::size > 0,
                  "quoll::rest::mount: a service returns a value for an endpoint without returns<>");

    using first = typename front<responses>::type;

    static_assert(std::same_as<T, typename first::type>, "quoll::rest::mount: a service returns the type of its "
                                                         "endpoint's first returns<>, or a reply<Endpoint>");

    return make_response<first>(value);
  }
}

// A service that returned nothing: 204 without returns<>, or a first response without body.
template <class Endpoint>
[[nodiscard]] http::response encode_void() {
  using responses = own_responses<Endpoint>;

  if constexpr (responses::size == 0) {
    http::response response;

    response.status = http::status::no_content;
    return response;
  } else {
    using first = typename front<responses>::type;

    static_assert(std::is_void_v<typename first::type>,
                  "quoll::rest::mount: a service returns nothing for an endpoint whose first returns<> has a body");

    return make_response<first>();
  }
}

// The service for an endpoint: the one invocable with it and its arguments.
template <class Endpoint, class... Arguments, class... Services>
[[nodiscard]] consteval std::size_t service_index(std::tuple<Arguments...> const *, type_list<Services...>) {
  return first_true(
      std::array<bool, sizeof...(Services)>{std::is_invocable_v<Services &, Endpoint, Arguments &&...>...});
}

template <class Endpoint, class... Arguments, class... Services>
[[nodiscard]] consteval std::size_t service_count(std::tuple<Arguments...> const *, type_list<Services...>) {
  return count_true(
      std::array<bool, sizeof...(Services)>{std::is_invocable_v<Services &, Endpoint, Arguments &&...>...});
}

template <class Route, class Validators, class... Services>
async::task<http::response> answer(mounted<Validators, Services...> &state, typename Route::arguments arguments) {
  using endpoint = typename Route::endpoint;
  using arguments_type = typename Route::arguments;

  constexpr arguments_type const *signature = nullptr;
  constexpr auto index = service_index<endpoint>(signature, type_list<Services...>{});

  static_assert(index < sizeof...(Services),
                "quoll::rest::mount: no service has an operator()(Endpoint, arguments...) for an endpoint of the api");
  static_assert(service_count<endpoint>(signature, type_list<Services...>{}) == 1,
                "quoll::rest::mount: more than one service takes an endpoint of the api");

  auto &service = std::get<index>(state.services);
  auto call = [&] {
    return std::apply([&](auto &...each) { return std::invoke(service, endpoint{}, std::move(each)...); }, arguments);
  };

  using result = decltype(call());

  if constexpr (task_value<result>::is_task) {
    if constexpr (std::is_void_v<typename task_value<result>::type>) {
      co_await call();
      co_return encode_void<endpoint>();
    } else {
      co_return encode_result<endpoint>(co_await call());
    }
  } else if constexpr (std::is_void_v<result>) {
    call();
    co_return encode_void<endpoint>();
  } else {
    co_return encode_result<endpoint>(call());
  }
}

// The answer to an http_error a service or a validator threw: its status if it is an error one, with the challenge
// of the route's first scheme on a 401 (RFC 9110); otherwise 500, as for any other exception.
template <class Route>
[[nodiscard]] http::response error_response(http_error const &error) {
  auto const code = static_cast<int>(error.status());

  if (code < 400 || code > 599) {
    return problem_response(http::status::internal_server_error, std::nullopt);
  }

  auto response = problem_response(error.status(), error.what());

  if constexpr (Route::schemes::size > 0) {
    if (error.status() == http::status::unauthorized) {
      response.fields.add("WWW-Authenticate",
                          challenge(std::type_identity<typename front<typename Route::schemes>::type>{}));
    }
  }

  return response;
}

// Serves a request routed to `Route`: credentials (401), arguments (400, 415), the service, its response. A service
// throwing http_error answers its status, anything else 500 without detail.
template <class Route, class State>
async::task<http::response> serve(std::shared_ptr<State> state, http::request request, http::route_match match) {
  try {
    // An invalid query string is reported after the credentials, which an api_key_query cannot then find.
    auto const query = parse_query(match.query);
    std::vector<query_entry> const none;
    request_input const input{request, match, query ? *query : none};

    if (auto refused = co_await check_security(typename Route::schemes{}, state->validators, input)) {
      co_return std::move(*refused);
    }

    if (!query) {
      co_return problem_response(http::status::bad_request, "invalid query string");
    }

    auto arguments = decode_arguments<Route>(input);

    if (!arguments) {
      co_return problem_response(arguments.error().status, std::move(arguments.error().detail));
    }

    co_return co_await answer<Route>(*state, std::move(*arguments));
  } catch (http_error const &error) {
    co_return error_response<Route>(error);
  } catch (...) {
    co_return problem_response(http::status::internal_server_error, std::nullopt);
  }
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_SERVE_HPP
