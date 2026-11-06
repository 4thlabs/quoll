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

#ifndef QUOLL_HTTP_DETAIL_TYPED_ROUTE_HPP
#define QUOLL_HTTP_DETAIL_TYPED_ROUTE_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/detail/number.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/route_match.hpp>
#include <quoll/http/status.hpp>

#include <algorithm>
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

namespace quoll::http::detail {

// What a path parameter can be read as.
template <class T>
concept PathParameter =
    std::same_as<T, std::string> || std::same_as<T, std::string_view> || std::same_as<T, bool> ||
    std::floating_point<T> || (std::integral<T> && !is_any_of<T, wchar_t, char8_t, char16_t, char32_t>);

// From a text that is the caller's to give away, and that outlives the handler's task: a view stays on it.
template <PathParameter T>
[[nodiscard]] std::optional<T> read_path_parameter(std::string &text) {
  if constexpr (std::same_as<T, std::string>) {
    return std::move(text);
  } else if constexpr (std::same_as<T, std::string_view>) {
    return T{text};
  } else if constexpr (std::same_as<T, bool>) {
    return parse_bool(text);
  } else {
    return parse_number<T>(text);
  }
}

template <class Result, class... Arguments>
struct signature {
  using result = Result;
  using arguments = std::tuple<Arguments...>;
};

// The signature of a handler: a function, or an object with a single operator(), which `auto` parameters do not give.
template <class T>
struct method_signature {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...)> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) const> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) noexcept> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) const noexcept> : signature<R, A...> {};

// A static operator(), as a `static` lambda has.
template <class R, class... A>
struct method_signature<R (*)(A...)> : signature<R, A...> {};

template <class R, class... A>
struct method_signature<R (*)(A...) noexcept> : signature<R, A...> {};

template <class F>
struct signature_of {};

template <class F>
  requires requires { &F::operator(); }
struct signature_of<F> : method_signature<decltype(&F::operator())> {};

template <class R, class... A>
struct signature_of<R (*)(A...)> : signature<R, A...> {};

template <class R, class... A>
struct signature_of<R (*)(A...) noexcept> : signature<R, A...> {};

template <class F>
concept HasSignature = requires { typename signature_of<F>::arguments; };

template <class T>
inline constexpr bool is_body_reader = std::same_as<std::remove_cvref_t<T>, body_reader>;

template <class Tuple>
inline constexpr std::size_t body_readers_in = 0;

template <class... Arguments>
inline constexpr std::size_t body_readers_in<std::tuple<Arguments...>> =
    (std::size_t{is_body_reader<Arguments>} + ... + 0);

// A handler taking an http::body_reader: its route reads the request as it goes.
template <class F>
inline constexpr bool reads_body_in_pieces = body_readers_in<typename signature_of<F>::arguments> != 0;

template <class T>
inline constexpr bool is_response_writer = std::same_as<std::remove_cvref_t<T>, response_writer>;

template <class Tuple>
inline constexpr std::size_t response_writers_in = 0;

template <class... Arguments>
inline constexpr std::size_t response_writers_in<std::tuple<Arguments...>> =
    (std::size_t{is_response_writer<Arguments>} + ... + 0);

template <class Tuple>
struct takes_request : std::false_type {};

template <class First, class... Rest>
struct takes_request<std::tuple<First, Rest...>> : std::is_same<std::remove_cvref_t<First>, request> {};

// What the compiler names in its error, as a consteval function cannot say more: see checked_path.
void the_route_template_is_invalid();
void the_only_type_a_path_says_is_path_as_the_handler_says_the_others();
void the_handler_does_not_take_as_many_parameters_as_the_path_has();

// A path checked at compile time against the handler it comes with, the way std::format_string checks a format against
// its arguments: `F` comes from the handler, and `std::type_identity_t` keeps the path from deducing it.
template <class F>
class checked_path {
  static_assert(HasSignature<F>, "quoll::http::router: a handler's parameters must have types, not auto");

public:
  // Implicit, and consteval: it is what makes the string literal of a call checked.
  template <std::size_t Size>
  consteval checked_path(char const (&path)[Size]) : text_(path, Size - 1) { // NOLINT(google-explicit-constructor)
    auto const parsed = parse_template(text_);

    if (text_.empty() || parsed.error != template_error::none) {
      the_route_template_is_invalid();
    }

    // A misspelt `{name:path}` would quietly take one segment.
    if (std::ranges::any_of(parsed.segments, [](template_segment const &segment) {
          return segment.kind == segment_kind::param && !segment.type.empty();
        })) {
      the_only_type_a_path_says_is_path_as_the_handler_says_the_others();
    }

    auto const in_path = static_cast<std::size_t>(std::ranges::count_if(
        parsed.segments, [](template_segment const &segment) { return segment.kind != segment_kind::literal; }));

    if (in_path != count()) {
      the_handler_does_not_take_as_many_parameters_as_the_path_has();
    }
  }

  [[nodiscard]] constexpr std::string_view text() const noexcept { return text_; }

private:
  [[nodiscard]] static consteval std::size_t count() {
    using arguments = typename signature_of<F>::arguments;

    return std::tuple_size_v<arguments> - (takes_request<arguments>::value ? 1 : 0) - body_readers_in<arguments> -
           response_writers_in<arguments>;
  }

  std::string_view text_;
};

template <class R>
inline constexpr bool is_response_result = std::same_as<R, response> || std::same_as<R, async::task<response>>;

// No request among the parameters of a handler.
struct no_request {};

// What a parameter of a handler is: read from `match`, or what reads the body.
template <class T, std::size_t Index>
[[nodiscard]] std::optional<T> read_parameter(route_match &match) {
  if constexpr (std::same_as<T, body_reader>) {
    return match.reader;
  } else {
    return read_path_parameter<T>(match.params.at(Index));
  }
}

// Calls `target` with its parameters read from `match`. Everything it is given lives in this frame until its task has
// ended, so that references and views are fine in a handler, a coroutine too.
template <class Result, class F, class Request, class... Parameters, std::size_t... Index>
async::task<response> call_typed(F &target, request message, route_match match, std::index_sequence<Index...>) {
  std::tuple<std::optional<std::remove_cvref_t<Parameters>>...> values{
      read_parameter<std::remove_cvref_t<Parameters>, Index>(match)...};

  if (!(std::get<Index>(values).has_value() && ...)) {
    co_return status_response(status::bad_request);
  }

  auto const call = [&]() -> Result {
    if constexpr (std::same_as<Request, no_request>) {
      return std::invoke(target, std::forward<Parameters>(*std::get<Index>(values))...);
    } else {
      return std::invoke(target, std::forward<Request>(message), std::forward<Parameters>(*std::get<Index>(values))...);
    }
  };

  if constexpr (std::same_as<Result, response>) {
    co_return call();
  } else {
    co_return co_await call();
  }
}

// What a handler writing through an http::response_writer is given, read once by the route and kept as it is, so that a
// view stays on what it refers to, until its task ends.
template <class... Parameters>
struct writing_frame {
  request message;
  route_match match;
  std::tuple<std::optional<std::remove_cvref_t<Parameters>>...> values{};
};

// Runs `action` when it goes, however its scope ends.
template <class F>
class at_scope_end {
public:
  explicit at_scope_end(F action) noexcept : action_(std::move(action)) {}

  at_scope_end(at_scope_end const &) = delete;
  at_scope_end &operator=(at_scope_end const &) = delete;
  at_scope_end(at_scope_end &&) = delete;
  at_scope_end &operator=(at_scope_end &&) = delete;

  ~at_scope_end() { action_(); }

private:
  F action_;
};

template <class F, class Request, class... Parameters, std::size_t... Index>
async::task<> run_writing(std::shared_ptr<writing_frame<Parameters...>> frame, std::shared_ptr<F> target,
                          body_stream out, std::index_sequence<Index...>) {
  auto const give_writer = [&]<class T, std::size_t At>() {
    if constexpr (std::same_as<T, response_writer>) {
      std::get<At>(frame->values).emplace(out);
    }
  };

  (give_writer.template operator()<std::remove_cvref_t<Parameters>, Index>(), ...);

  // Gone with its handler, however it ends: it gives its head to what it writes through, which does not outlive this.
  auto const drop_writer = [&]<class T, std::size_t At>() {
    if constexpr (std::same_as<T, response_writer>) {
      std::get<At>(frame->values).reset();
    }
  };

  at_scope_end const dropping{
      [&] { (drop_writer.template operator()<std::remove_cvref_t<Parameters>, Index>(), ...); }};

  if constexpr (std::same_as<Request, no_request>) {
    co_await std::invoke(*target, std::forward<Parameters>(*std::get<Index>(frame->values))...);
  } else {
    co_await std::invoke(*target, std::forward<Request>(frame->message),
                         std::forward<Parameters>(*std::get<Index>(frame->values))...);
  }
}

// The answer of such a route: its parameters read, and its handler to run as the body's writer, which holds it, as a
// response may go on after its router.
template <class F, class Request, class... Parameters, std::size_t... Index>
[[nodiscard]] response answer_writing(std::shared_ptr<F> const &target, request message, route_match match,
                                      std::index_sequence<Index...> indices) {
  auto frame = std::make_shared<writing_frame<Parameters...>>(std::move(message), std::move(match));
  auto const read = [&]<class T, std::size_t At>() {
    if constexpr (!std::same_as<T, response_writer>) {
      std::get<At>(frame->values) = read_parameter<T, At>(frame->match);
    }
  };

  (read.template operator()<std::remove_cvref_t<Parameters>, Index>(), ...);

  if (!((is_response_writer<Parameters> || std::get<Index>(frame->values).has_value()) && ...)) {
    return status_response(status::bad_request);
  }

  response answer;

  answer.head_comes_with_the_body = true;
  answer.body_writer = [frame = std::move(frame), target, indices](body_stream out) {
    return run_writing<F, Request, Parameters...>(frame, target, out, indices);
  };
  return answer;
}

template <class... Parameters>
[[nodiscard]] consteval bool body_reader_comes_last() {
  auto reader_seen = false;
  auto in_order = true;

  // Once one is found, nothing may follow it but what writes the response.
  ((in_order = in_order && (!reader_seen || is_response_writer<Parameters>),
    reader_seen = reader_seen || is_body_reader<Parameters>),
   ...);

  return in_order;
}

template <class... Parameters>
[[nodiscard]] consteval bool response_writer_comes_last() {
  auto left = sizeof...(Parameters);

  return (((left--, !is_response_writer<Parameters>) || left == 0) && ...);
}

template <class Result, class F, class Request, class... Parameters>
[[nodiscard]] auto typed_handler_for(F target) {
  constexpr auto writes = (is_response_writer<Parameters> || ...);

  static_assert(writes || is_response_result<Result>,
                "quoll::http::router: a handler returns http::response or task<http::response>");
  static_assert(!writes || std::same_as<Result, async::task<>>,
                "quoll::http::router: a handler taking an http::response_writer returns task<>");
  static_assert(((PathParameter<std::remove_cvref_t<Parameters>> || is_body_reader<Parameters> ||
                  is_response_writer<Parameters>) &&
                 ...),
                "quoll::http::router: a path parameter is a std::string, a std::string_view, a number or a bool");
  static_assert(body_reader_comes_last<Parameters...>(),
                "quoll::http::router: a handler takes one http::body_reader, last");
  static_assert(response_writer_comes_last<Parameters...>(),
                "quoll::http::router: a handler takes one http::response_writer, last");

  if constexpr (writes) {
    return
        [target = std::make_shared<F>(std::move(target))](request message, route_match match) -> async::task<response> {
          co_return answer_writing<F, Request, Parameters...>(target, std::move(message), std::move(match),
                                                              std::index_sequence_for<Parameters...>{});
        };
  } else {
    return [target = std::move(target)](request message, route_match match) mutable -> async::task<response> {
      return call_typed<Result, F, Request, Parameters...>(target, std::move(message), std::move(match),
                                                           std::index_sequence_for<Parameters...>{});
    };
  }
}

template <class F, class Result, class First, class... Rest>
[[nodiscard]] auto typed_handler_from(F target, std::tuple<First, Rest...> const *) {
  if constexpr (std::same_as<std::remove_cvref_t<First>, request>) {
    return typed_handler_for<Result, F, First, Rest...>(std::move(target));
  } else {
    return typed_handler_for<Result, F, no_request, First, Rest...>(std::move(target));
  }
}

template <class F, class Result>
[[nodiscard]] auto typed_handler_from(F target, std::tuple<> const *) {
  return typed_handler_for<Result, F, no_request>(std::move(target));
}

// A handler of the router, from one that takes its parameters: `F` has a signature, as checked_path<F> says.
template <class F>
[[nodiscard]] auto typed_handler(F target) {
  using traits = signature_of<F>;

  return typed_handler_from<F, typename traits::result>(std::move(target),
                                                        static_cast<typename traits::arguments const *>(nullptr));
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_TYPED_ROUTE_HPP
