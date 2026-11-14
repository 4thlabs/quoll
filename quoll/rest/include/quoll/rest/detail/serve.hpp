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

#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/detail/error_answer.hpp>
#include <quoll/http/detail/query_string.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/problem.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/credential.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/query.hpp>
#include <quoll/rest/detail/scheme_traits.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/event_sink.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/query_params.hpp>
#include <quoll/rest/security.hpp>
#include <quoll/rest/validators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
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

template <class Scheme>
[[nodiscard]] http::response unauthorized(std::string detail) {
  auto response = http::problem_response(http::status::unauthorized, std::move(detail));

  response.fields.add("WWW-Authenticate", scheme_traits<Scheme>::challenge());
  return response;
}

// The validator taking the credentials of `Scheme`: the one invocable with them.
template <class Scheme, class... Validators>
[[nodiscard]] consteval std::size_t validator_index(type_list<Validators...>) {
  return first_true(
      std::array<bool, sizeof...(Validators)>{std::is_invocable_v<Validators &, credential<Scheme> const &>...}
  );
}

template <class Scheme, class... Validators>
[[nodiscard]] consteval std::size_t validator_count(type_list<Validators...>) {
  return count_true(
      std::array<bool, sizeof...(Validators)>{std::is_invocable_v<Validators &, credential<Scheme> const &>...}
  );
}

template <class Scheme, class... Validators>
async::task<bool> accepts(validator_set<Validators...> &set, credential<Scheme> const value) {
  constexpr auto index = validator_index<Scheme>(type_list<Validators...>{});

  static_assert(
      index < sizeof...(Validators),
      "quoll::rest::mount: no validator takes the credential<Scheme> of a scheme the api uses"
  );
  // Whichever came first would decide who gets in, a generic one in the place of the one written for the scheme.
  static_assert(
      validator_count<Scheme>(type_list<Validators...>{}) == 1,
      "quoll::rest::mount: more than one validator takes the credential<Scheme> of a scheme the api uses"
  );

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
async::task<std::optional<http::response>>
check_security(type_list<Scheme, Rest...>, Validators &validators, request_input const &input) {
  auto value = scheme_traits<Scheme>::find(input.request.fields, input.query);

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
  typename Response::codec const codec{};
  auto response = make_response<Response>();

  response.body = codec.encode(value);
  response.fields.add("Content-Type", codec.content_type);
  return response;
}

// What a service returned: the value of its route's result.
template <class Endpoint, class T>
[[nodiscard]] http::response encode_result(T const &value) {
  using responses = own_responses<Endpoint>;

  static_assert(responses::size > 0, "quoll::rest::mount: a handler returns a value for a route whose result is void");

  using first = typename front<responses>::type;

  static_assert(
      std::same_as<T, typename first::type>, "quoll::rest::mount: a handler returns the type of its route's result"
  );

  return make_response<first>(value);
}

// A service that returned nothing: 204 for a void result, or a result without body.
template <class Endpoint>
[[nodiscard]] http::response encode_void() {
  using responses = own_responses<Endpoint>;

  if constexpr (responses::size == 0) {
    http::response response;

    response.status = http::status::no_content;
    return response;
  } else {
    using first = typename front<responses>::type;

    static_assert(
        std::is_void_v<typename first::type>,
        "quoll::rest::mount: a handler returns nothing for a route whose result has a body"
    );

    return make_response<first>();
  }
}

// An argument, while a service is looked for: it converts to `T`, and to nothing else. A service taking a narrower
// type, or its arguments in another order, would otherwise compile and read wrong values.
template <class T>
struct exactly {
  operator T &&() &&; // NOLINT(google-explicit-constructor)

  template <class Other>
    requires(!std::same_as<std::remove_cvref_t<Other>, T>)
  operator Other() && = delete;
};

// The service for an endpoint: the one taking it and exactly its arguments, by value or by reference.
// `Extra`, given as is after the arguments: the sink of an endpoint with events<>.
template <class Endpoint, class... Extra, class... Arguments, class... Services>
[[nodiscard]] consteval std::size_t service_index(std::tuple<Arguments...> const *, type_list<Services...>) {
  return first_true(std::array<bool, sizeof...(Services)>{
      std::is_invocable_v<Services &, Endpoint, exactly<Arguments>..., Extra...>...
  });
}

template <class Endpoint, class... Extra, class... Arguments, class... Services>
[[nodiscard]] consteval std::size_t service_count(std::tuple<Arguments...> const *, type_list<Services...>) {
  return count_true(std::array<bool, sizeof...(Services)>{
      std::is_invocable_v<Services &, Endpoint, exactly<Arguments>..., Extra...>...
  });
}

template <class Route, class Validators, class... Services>
async::task<http::response> answer(mounted<Validators, Services...> &state, typename Route::arguments arguments) {
  using endpoint = typename Route::endpoint;
  using arguments_type = typename Route::arguments;

  constexpr arguments_type const *signature = nullptr;
  constexpr auto index = service_index<endpoint>(signature, type_list<Services...>{});

  // mount() has checked it is there, once (route_has_one_handler).
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

// The sink the service of an endpoint with events<> sends through.
template <class Endpoint>
using sink_of = event_sink<typename events_part_of<Endpoint>::type, typename events_part_of<Endpoint>::codec>;

// The answer to an http_error a service or a validator threw, as the router answers one (http::detail::error_answer),
// with the challenge of the route's first scheme on a 401 (RFC 9110); none for one raised with a status that is no
// error, which goes on as any other exception.
template <class Route>
[[nodiscard]] std::optional<http::response> error_response(http::http_error const &error) {
  auto answer = http::detail::error_answer(error);

  if constexpr (Route::schemes::size > 0) {
    if (answer && !error.received() && error.status() == http::status::unauthorized) {
      answer->fields.add("WWW-Authenticate", scheme_traits<typename front<typename Route::schemes>::type>::challenge());
    }
  }

  return answer;
}

// What the arguments of a request are decoded from: for a stream, it lives as long as the stream, as they may point
// into it.
struct served_input {
  http::request request;
  http::route_match match;
  std::optional<std::vector<http::detail::query_entry>> query;
};

// The services taking an endpoint's arguments with no `Sink`, and not with one: those a stream would never call.
template <class Endpoint, class Sink, class... Arguments, class... Services>
[[nodiscard]] consteval std::size_t services_without_sink(std::tuple<Arguments...> const *, type_list<Services...>) {
  return count_true(std::array<bool, sizeof...(Services)>{
      (std::is_invocable_v<Services &, Endpoint, exactly<Arguments>...> &&
       !std::is_invocable_v<Services &, Endpoint, exactly<Arguments>..., Sink>)...
  });
}

// The arguments of a stream, and whether it has run: it runs once, as a handler writing its response does.
template <class Arguments>
struct held_arguments {
  // Declared first: destroyed after the arguments, which may point into it.
  std::shared_ptr<served_input const> input;
  Arguments values;
  std::atomic_flag started;
};

// The stream of an endpoint with events<>: its service, given a sink over the response, run as the response's writer,
// after the router has answered. An http_error before the stream starts answers as from any service; after, it cuts the
// stream, as any failure of a writer does. What it reads is held here: the arguments, and the state of the services.
template <class Route, class Validators, class... Services>
[[nodiscard]] async::task<> stream_events(
    std::shared_ptr<mounted<Validators, Services...>> state,
    std::shared_ptr<held_arguments<typename Route::arguments>> held, http::body_stream out
) {
  if (held->started.test_and_set()) {
    throw std::logic_error{"quoll::rest::mount: the stream of a response run again"};
  }

  using endpoint = typename Route::endpoint;
  using sink_type = sink_of<endpoint>;
  using arguments_type = typename Route::arguments;
  constexpr arguments_type const *signature = nullptr;
  constexpr auto index = service_index<endpoint, sink_type &>(signature, type_list<Services...>{});

  // mount() has checked it is there, once (route_has_one_handler).
  auto &service = std::get<index>(state->services);
  sink_type sink{http::response_writer{out}};
  // Closed on every way out, a throw included, while the connection is there: an operation kept past the service, run
  // or dropped later, never reaches it.
  auto const closing = event_sink_access::closing(sink);
  auto call = [&] {
    return std::apply(
        [&](auto &...each) { return std::invoke(service, endpoint{}, std::move(each)..., sink); }, held->values
    );
  };

  static_assert(
      std::same_as<decltype(call()), async::task<>>,
      "quoll::rest::mount: the handler of a route with events<> returns a task<>"
  );

  std::optional<http::response> refused;
  std::exception_ptr failure;
  auto stopped = false;

  // A stop seen here rather than ending this frame: the drain below runs whatever ended the service.
  try {
    stopped = !co_await async::stopped_as_optional(call());
  } catch (http::http_error const &error) {
    refused = error_response<Route>(error);

    failure = std::current_exception();
  } catch (...) {
    failure = std::current_exception();
  }

  // Closed to operations not running yet, and those running ended: they write to the connection, which goes with
  // the stream. Then whether it has started is known for good.
  co_await event_sink_access::drain(sink);

  if (stopped) {
    co_await async::detail::end_stopped{};
  }

  if (refused && !sink.started()) {
    co_await event_sink_access::answer(sink, std::move(*refused));
    co_return;
  }

  if (failure) {
    std::rethrow_exception(failure);
  }

  // A stream with no event is still one: its head, so that a client reads it as such, and its end.
  co_await event_sink_access::finish(sink);
}

// The answer of an endpoint with events<>: its stream, run once the router has answered.
template <class Route, class State>
[[nodiscard]] http::response events_response(
    std::shared_ptr<State> state, std::shared_ptr<served_input const> input, typename Route::arguments arguments
) {
  auto held = std::make_shared<held_arguments<typename Route::arguments>>(std::move(input), std::move(arguments));
  http::response response;

  response.late_head_writer = [state = std::move(state), held = std::move(held)](http::body_stream out) {
    return stream_events<Route>(state, held, out);
  };
  return response;
}

// Whether a route reads the query: a query it makes nothing of, however it is written, is none of its business.
template <class Part>
inline constexpr bool reads_the_query = false;

template <fixed_string Name, class T>
inline constexpr bool reads_the_query<query<Name, T>> = true;

template <class Struct>
inline constexpr bool reads_the_query<query_params<Struct>> = true;

template <fixed_string Param>
inline constexpr bool reads_the_query<api_key_query<Param>> = true;

template <class... Parts>
[[nodiscard]] consteval bool any_reads_the_query(type_list<Parts...>) {
  return (reads_the_query<Parts> || ...);
}

// One frame for a request, which holds what its arguments are decoded from: in it, or shared with the stream of an
// endpoint with events<>, as they may point into it.
template <class Route, class State, class Input>
[[nodiscard]] async::task<http::response> serve_from(std::shared_ptr<State> state, Input input) {
  // What `input` holds, as it is or behind the pointer of a stream.
  auto &in = [&]() -> served_input & {
    if constexpr (std::same_as<Input, served_input>) {
      return input;
    } else {
      return *input;
    }
  }();

  try {
    constexpr auto reads_query =
        any_reads_the_query(typename Route::parts{}) || any_reads_the_query(typename Route::schemes{});

    // An invalid query string is reported after the credentials, which an api_key_query cannot then find.
    in.query = reads_query ? http::detail::parse_query(in.match.query)
                           : std::optional{std::vector<http::detail::query_entry>{}};

    std::vector<http::detail::query_entry> const none;
    request_input const request{in.request, in.match, in.query ? *in.query : none};

    if constexpr (Route::schemes::size > 0) {
      if (auto refused = co_await check_security(typename Route::schemes{}, state->validators, request)) {
        co_return std::move(*refused);
      }
    }

    if (!in.query) {
      co_return http::problem_response(http::status::bad_request, "invalid query string");
    }

    auto arguments = decode_arguments<Route>(request);

    if (!arguments) {
      co_return http::problem_response(arguments.error().status, std::move(arguments.error().detail));
    }

    if constexpr (has_events<typename Route::endpoint>) {
      // Its name is the router's, which the stream may outlive.
      in.match.name = {};
      co_return events_response<Route>(state, std::move(input), std::move(*arguments));
    } else {
      co_return co_await answer<Route>(*state, std::move(*arguments));
    }
  } catch (http::http_error const &error) {
    auto answer = error_response<Route>(error);

    if (!answer) {
      throw;
    }

    co_return std::move(*answer);
  }
}

// Serves a request routed to `Route`: credentials (401), arguments (400, 415), the service, its response. A service
// throwing http_error with an error status answers it. Anything else goes on, as from any handler: a connection answers
// 500 and tells on_handler_error, and a stop or a timeout reaches what waits for it.
template <class Route, class State>
[[nodiscard]] async::task<http::response>
serve(std::shared_ptr<State> state, http::request request, http::route_match match) {
  if constexpr (has_events<typename Route::endpoint>) {
    return serve_from<Route>(
        std::move(state), std::make_shared<served_input>(std::move(request), std::move(match), std::nullopt)
    );
  } else {
    return serve_from<Route>(std::move(state), served_input{std::move(request), std::move(match), std::nullopt});
  }
}

// Whether `Route` has one handler among `Services`, the route first, then exactly its arguments, and its sink for one
// of events<>: mount() checks it before it instantiates what serves the route, whose errors would then not be the
// cause.
template <class Route, class... Services>
[[nodiscard]] consteval bool route_has_one_handler(type_list<Services...> /*services*/) {
  using endpoint = typename Route::endpoint;
  constexpr typename Route::arguments const *signature = nullptr;

  if constexpr (has_events<endpoint>) {
    using sink_type = sink_of<endpoint> &;

    constexpr bool found =
        service_index<endpoint, sink_type>(signature, type_list<Services...>{}) < sizeof...(Services);
    // A second one taking no sink counts too, which would never be called, but not the same one twice.
    constexpr bool once = service_count<endpoint, sink_type>(signature, type_list<Services...>{}) +
                              services_without_sink<endpoint, sink_type>(signature, type_list<Services...>{}) ==
                          1;

    static_assert(
        found,
        "quoll::rest::mount: no handler takes a route of the api with events<>, the route first, then exactly its "
        "arguments and its event_sink<T> &"
    );
    static_assert(!found || once, "quoll::rest::mount: more than one handler takes a route of the api");
    return found && once;
  } else {
    constexpr bool found = service_index<endpoint>(signature, type_list<Services...>{}) < sizeof...(Services);
    constexpr bool once = service_count<endpoint>(signature, type_list<Services...>{}) == 1;

    static_assert(
        found, "quoll::rest::mount: no handler takes a route of the api, the route first, then exactly its arguments"
    );
    static_assert(!found || once, "quoll::rest::mount: more than one handler takes a route of the api");
    return found && once;
  }
}

template <class Route, class Validators, class... Services>
[[nodiscard]] consteval bool route_has_one_handler(std::type_identity<mounted<Validators, Services...>>) {
  return route_has_one_handler<Route>(type_list<Services...>{});
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_SERVE_HPP
