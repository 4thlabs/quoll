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

#ifndef QUOLL_REST_DETAIL_CLIENT_REQUEST_HPP
#define QUOLL_REST_DETAIL_CLIENT_REQUEST_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/rest/body.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/fields_of.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/http/detail/query_string.hpp>
#include <quoll/rest/detail/scheme_traits.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/param_traits.hpp>
#include <quoll/rest/query.hpp>
#include <quoll/rest/security.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

// The credentials a client was given, by scheme.
using credential_map = std::map<std::string, std::string, std::less<>>;

// A request under construction: its query string and fields grow part by part.
struct request_draft {
  std::string query;
  http::request request;
};

template <fixed_string Name, class T>
void add_part(std::type_identity<query<Name, T>>, T const &value, request_draft &draft) {
  for_each_text(value, [&](std::string const &text) { http::detail::append_query(draft.query, Name.view(), text); });
}

template <class Struct>
void add_part(std::type_identity<query_params<Struct>>, Struct const &value, request_draft &draft) {
  append_fields(draft.query, value, "quoll::rest::client: query parameter");
}

// A header value the server reads back as it is: no surrounding whitespace, which parsers trim, and in a list header
// no comma, where the server splits, nor an empty element, which it skips.
template <class T>
void check_header_text(std::string_view name, std::string_view text) {
  auto const trimmed = http::detail::trim(text) == text;
  auto const listed = !is_vector<T>::value || (!text.empty() && !text.contains(','));

  if (!trimmed || !listed) {
    throw std::invalid_argument{
        "quoll::rest::client: header " + std::string{name} + " has a value the server would read differently"
    };
  }
}

template <fixed_string Name, class T>
void add_part(std::type_identity<header<Name, T>>, T const &value, request_draft &draft) {
  for_each_text(value, [&](std::string const &text) {
    check_header_text<T>(Name.view(), text);
    draft.request.fields.add(Name.view(), text);
  });
}

template <class T, class BodyCodec>
void add_part(std::type_identity<body<T, BodyCodec>>, T const &value, request_draft &draft) {
  BodyCodec const codec{};

  draft.request.body = codec.encode(value);
  draft.request.fields.add("Content-Type", codec.content_type);
}

// The credentials the client has for the schemes of a route; a scheme without one is left out.
template <class... Scheme>
void add_credentials(type_list<Scheme...>, credential_map const &credentials, request_draft &draft) {
  (
      [&] {
        if (auto const found = credentials.find(scheme_traits<Scheme>::key.view()); found != credentials.end()) {
          scheme_traits<Scheme>::add(found->second, draft.request.fields, draft.query);
        }
      }(),
      ...
  );
}

// A path parameter the server reads back as it is, and that stays where the template puts it. Not empty, for a single
// segment; for the rest of a path, which goes out with its '/', not starting with one, as `//host` reads as another
// server. And what http::detail::plain_segment says: no NUL, and no `.` or `..` part, which a URL parser, or a proxy
// decoding what the encoding hides, resolves, so a request could reach another endpoint with the client's credentials.
// A '/' elsewhere in a single segment stays: it goes encoded, and some APIs ask for it (`group%2Fproject`).
void check_path_text(std::string_view name, std::string_view text, bool rest_of_path);

template <class Route, std::size_t Index>
[[nodiscard]] std::string path_text(typename Route::arguments const &arguments) {
  constexpr auto param = checked_path<Route::path_value>::parsed.params[Index];
  constexpr bool rest_of_path = param.kind == param_kind::rest_of_path;

  auto text = param_traits<std::tuple_element_t<Index, typename Route::arguments>>::to_text(std::get<Index>(arguments));

  check_path_text(param.name, text, rest_of_path);
  return http::detail::percent_encode(text, rest_of_path ? "/" : "");
}

// The path of a route with its parameters: each one's text, percent-encoded; the rest of a path keeps its '/'.
template <class Route, std::size_t... Index>
[[nodiscard]] std::string fill_path(typename Route::arguments const &arguments, std::index_sequence<Index...>) {
  std::array<std::string, sizeof...(Index)> const texts{path_text<Route, Index>(arguments)...};
  std::string_view const path = Route::path;
  std::string filled;
  std::size_t next = 0;

  for (std::size_t index = 0; index < path.size(); ++index) {
    if (path[index] != '{') {
      filled += path[index];
      continue;
    }

    filled += texts[next++];
    index = path.find('}', index);
  }

  return filled;
}

template <class Route, class Parts, std::size_t... Index>
void add_parts(typename Route::arguments const &arguments, request_draft &draft, Parts, std::index_sequence<Index...>) {
  constexpr auto first = checked_path<Route::path_value>::parsed.size;

  [&]<class... Part>(type_list<Part...>) {
    (add_part(std::type_identity<Part>{}, std::get<first + Index>(arguments), draft), ...);
  }(Parts{});
}

// The request of a call: method, target with the path parameters and the query, fields, body, and the credentials
// the client has for the route's schemes (a scheme without one is left out: the server answers 401).
template <class Route>
[[nodiscard]] http::request
build_request(typename Route::arguments const &arguments, credential_map const &credentials) {
  using argument_parts = filter_t<is_argument_part, typename Route::parts>;

  constexpr auto path_size = checked_path<Route::path_value>::parsed.size;

  request_draft draft;

  draft.request.method = Route::endpoint::method;
  add_parts<Route>(arguments, draft, argument_parts{}, std::make_index_sequence<argument_parts::size>{});

  add_credentials(typename Route::schemes{}, credentials, draft);

  draft.request.target = fill_path<Route>(arguments, std::make_index_sequence<path_size>{});

  if (!draft.query.empty()) {
    draft.request.target += '?' + draft.query;
  }

  return std::move(draft.request);
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CLIENT_REQUEST_HPP
