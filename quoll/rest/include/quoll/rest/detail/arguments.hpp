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

#ifndef QUOLL_REST_DETAIL_ARGUMENTS_HPP
#define QUOLL_REST_DETAIL_ARGUMENTS_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest/body.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/fields_of.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/query_string.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/param_traits.hpp>
#include <quoll/rest/query.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

// Why a request does not carry the arguments of its endpoint: 400, or 415 for a body of another media type.
struct argument_error {
  http::status status = http::status::bad_request;
  std::string detail;
};

// What the arguments are decoded from.
struct request_input {
  http::request const &request;
  http::route_match const &match;
  std::vector<query_entry> const &query;
};

template <class T>
struct is_optional : std::false_type {};

template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

// What a parameter's text decodes to: the value of an optional, the type itself otherwise.
template <class T>
struct optional_value {
  using type = T;
};

template <class T>
struct optional_value<std::optional<T>> {
  using type = T;
};

template <class T>
struct is_vector : std::false_type {};

template <class T>
struct is_vector<std::vector<T>> : std::true_type {};

// A query or header value from the texts the request has for it: a vector takes them all, an optional none or one,
// any other type exactly one.
template <class T>
[[nodiscard]] std::expected<T, argument_error> from_texts(std::string_view what, std::string_view name,
                                                          std::vector<std::string_view> const &texts) {
  auto const invalid = [&] {
    return std::unexpected{
        argument_error{http::status::bad_request, "invalid " + std::string{what} + " " + std::string{name}}};
  };

  if constexpr (is_vector<T>::value) {
    T values;

    for (auto const text : texts) {
      auto value = param_traits<typename T::value_type>::from_text(text);

      if (!value) {
        return invalid();
      }

      values.push_back(std::move(*value));
    }

    return values;
  } else {
    if (texts.size() > 1) {
      return std::unexpected{
          argument_error{http::status::bad_request, std::string{what} + " " + std::string{name} + " is repeated"}};
    }

    if (texts.empty()) {
      if constexpr (is_optional<T>::value) {
        return T{};
      } else {
        return std::unexpected{
            argument_error{http::status::bad_request, "missing " + std::string{what} + " " + std::string{name}}};
      }
    }

    auto value = param_traits<typename optional_value<T>::type>::from_text(texts.front());

    if (!value) {
      return invalid();
    }

    return T{std::move(*value)};
  }
}

[[nodiscard]] std::vector<std::string_view> header_texts(http::fields const &fields, std::string_view name);

// The media type of a Content-Type value, without its parameters: "application/json; charset=utf-8" gives
// "application/json".
[[nodiscard]] std::string_view media_type(std::string_view content_type) noexcept;

// A path parameter of the route's template, by index.
template <fixed_string Path, std::size_t Index>
struct path_source {
  static constexpr auto param = checked_path<Path>::parsed.params[Index];

  using value_type = typename param_kind_type<param.kind>::type;
};

template <fixed_string Path, std::size_t Index>
[[nodiscard]] std::expected<typename path_source<Path, Index>::value_type, argument_error>
decode_source(std::type_identity<path_source<Path, Index>>, request_input const &input) {
  using source = path_source<Path, Index>;

  auto value = param_traits<typename source::value_type>::from_text(input.match.params[Index]);

  if (!value) {
    return std::unexpected{
        argument_error{http::status::bad_request, "invalid path parameter " + std::string{source::param.name}}};
  }

  return std::move(*value);
}

template <fixed_string Name, class T>
[[nodiscard]] std::expected<T, argument_error> decode_source(std::type_identity<query<Name, T>>,
                                                             request_input const &input) {
  return from_texts<T>("query parameter", Name.view(), query_texts(input.query, Name.view()));
}

template <class Struct>
[[nodiscard]] std::expected<Struct, argument_error> decode_source(std::type_identity<query_params<Struct>>,
                                                                  request_input const &input) {
  Struct value{};
  std::optional<argument_error> error;

  for_each_field(value, [&](std::string_view name, auto &field) {
    if (error) {
      return;
    }

    auto decoded =
        from_texts<std::remove_cvref_t<decltype(field)>>("query parameter", name, query_texts(input.query, name));

    if (!decoded) {
      error = std::move(decoded.error());
      return;
    }

    field = std::move(*decoded);
  });

  if (error) {
    return std::unexpected{std::move(*error)};
  }

  return value;
}

// The elements of a list header: its field lines, each split at commas, as RFC 9110 lets a sender or a proxy combine
// repeated fields into one; empty elements are skipped.
[[nodiscard]] std::vector<std::string_view> list_elements(std::vector<std::string_view> const &lines);

template <fixed_string Name, class T>
[[nodiscard]] std::expected<T, argument_error> decode_source(std::type_identity<header<Name, T>>,
                                                             request_input const &input) {
  auto lines = header_texts(input.request.fields, Name.view());

  if constexpr (is_vector<T>::value) {
    lines = list_elements(lines);
  }

  return from_texts<T>("header", Name.view(), lines);
}

template <class T, class BodyCodec>
[[nodiscard]] std::expected<T, argument_error> decode_source(std::type_identity<body<T, BodyCodec>>,
                                                             request_input const &input) {
  constexpr BodyCodec codec;
  auto const content_type = input.request.fields.find("Content-Type");

  if (!content_type && input.request.body.empty()) {
    return std::unexpected{argument_error{http::status::bad_request, "missing body"}};
  }

  if (!content_type || !http::detail::iequals(media_type(*content_type), media_type(codec.content_type))) {
    return std::unexpected{argument_error{http::status::unsupported_media_type,
                                          "the body is " + std::string{media_type(codec.content_type)}}};
  }

  auto value = codec.template decode<T>(input.request.body);

  if (!value) {
    return std::unexpected{argument_error{http::status::bad_request, "invalid body: " + value.error().message}};
  }

  return std::move(*value);
}

template <fixed_string Path, class Indices = std::make_index_sequence<checked_path<Path>::parsed.size>>
struct path_sources;

template <fixed_string Path, std::size_t... Index>
struct path_sources<Path, std::index_sequence<Index...>> {
  using type = type_list<path_source<Path, Index>...>;
};

template <class Source>
struct source_value {
  using type = typename Source::argument_type;
};

template <fixed_string Path, std::size_t Index>
struct source_value<path_source<Path, Index>> {
  using type = typename path_source<Path, Index>::value_type;
};

// A route without arguments.
[[nodiscard]] std::expected<std::tuple<>, argument_error> decode_all(type_list<>, request_input const &);

// Decodes every source in order, stopping at the first error.
template <class First, class... Sources>
[[nodiscard]] std::expected<std::tuple<typename source_value<First>::type, typename source_value<Sources>::type...>,
                            argument_error>
decode_all(type_list<First, Sources...>, request_input const &input) {
  std::optional<argument_error> error;

  auto decode = [&]<class Source>(std::type_identity<Source> source) {
    std::optional<typename source_value<Source>::type> value;

    if (error) {
      return value;
    }

    if (auto decoded = decode_source(source, input)) {
      value.emplace(std::move(*decoded));
    } else {
      error = std::move(decoded.error());
    }

    return value;
  };

  // Braced: the sources are decoded in order.
  std::tuple<std::optional<typename source_value<First>::type>, std::optional<typename source_value<Sources>::type>...>
      values{decode(std::type_identity<First>{}), decode(std::type_identity<Sources>{})...};

  if (error) {
    return std::unexpected{std::move(*error)};
  }

  return std::apply([](auto &...value) { return std::tuple{std::move(*value)...}; }, values);
}

// The arguments of a route, decoded from a request: path parameters, then the argument parts in order.
template <class Route>
[[nodiscard]] std::expected<typename Route::arguments, argument_error> decode_arguments(request_input const &input) {
  using sources =
      concat_t<typename path_sources<Route::path_value>::type, filter_t<is_argument_part, typename Route::parts>>;

  return decode_all(sources{}, input);
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_ARGUMENTS_HPP
