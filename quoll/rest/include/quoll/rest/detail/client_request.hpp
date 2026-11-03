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

#include <quoll/http/detail/percent.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/rest/body.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/fields_of.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/header.hpp>
#include <quoll/rest/param_traits.hpp>
#include <quoll/rest/query.hpp>
#include <quoll/rest/security.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

// The credentials a client was given, by scheme.
using credential_map = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] inline std::string scheme_key(std::type_identity<bearer_auth>) { return "bearer"; }

template <fixed_string Header>
[[nodiscard]] std::string scheme_key(std::type_identity<api_key<Header>>) {
  return "header:" + std::string{Header.view()};
}

template <fixed_string Param>
[[nodiscard]] std::string scheme_key(std::type_identity<api_key_query<Param>>) {
  return "query:" + std::string{Param.view()};
}

// A request under construction: its query string and fields grow part by part.
struct request_draft {
  std::string query;
  http::request request;
};

inline void add_query(request_draft &draft, std::string_view name, std::string_view value) {
  draft.query += draft.query.empty() ? "" : "&";
  draft.query += http::detail::percent_encode(name);
  draft.query += '=';
  draft.query += http::detail::percent_encode(value);
}

// Calls `add(text)` for each text of a query or header value: every element of a vector, the value of an optional
// that has one, the value itself otherwise.
template <class T, class Add>
void for_each_text(T const &value, Add &&add) {
  if constexpr (is_vector<T>::value) {
    for (auto const &element : value) {
      add(param_traits<typename T::value_type>::to_text(element));
    }
  } else if constexpr (is_optional<T>::value) {
    if (value) {
      add(param_traits<typename T::value_type>::to_text(*value));
    }
  } else {
    add(param_traits<T>::to_text(value));
  }
}

template <fixed_string Name, class T>
void add_part(std::type_identity<query<Name, T>>, T const &value, request_draft &draft) {
  for_each_text(value, [&](std::string const &text) { add_query(draft, Name.view(), text); });
}

template <class Struct>
void add_part(std::type_identity<query_params<Struct>>, Struct const &value, request_draft &draft) {
  for_each_field(value, [&](std::string_view name, auto const &field) {
    for_each_text(field, [&](std::string const &text) { add_query(draft, name, text); });
  });
}

template <fixed_string Name, class T>
void add_part(std::type_identity<header<Name, T>>, T const &value, request_draft &draft) {
  for_each_text(value, [&](std::string const &text) { draft.request.fields.add(Name.view(), text); });
}

template <class T, class BodyCodec>
void add_part(std::type_identity<body<T, BodyCodec>>, T const &value, request_draft &draft) {
  constexpr BodyCodec codec;

  draft.request.body = codec.encode(value);
  draft.request.fields.add("Content-Type", codec.content_type);
}

inline void add_credential(std::type_identity<bearer_auth>, std::string const &value, request_draft &draft) {
  draft.request.fields.add("Authorization", "Bearer " + value);
}

template <fixed_string Header>
void add_credential(std::type_identity<api_key<Header>>, std::string const &value, request_draft &draft) {
  draft.request.fields.add(Header.view(), value);
}

template <fixed_string Param>
void add_credential(std::type_identity<api_key_query<Param>>, std::string const &value, request_draft &draft) {
  add_query(draft, Param.view(), value);
}

// The credentials the client has for the schemes of a route; a scheme without one is left out.
template <class... Scheme>
void add_credentials(type_list<Scheme...>, credential_map const &credentials, request_draft &draft) {
  (
      [&] {
        if (auto const found = credentials.find(scheme_key(std::type_identity<Scheme>{})); found != credentials.end()) {
          add_credential(std::type_identity<Scheme>{}, found->second, draft);
        }
      }(),
      ...);
}

// The path of a route with its parameters: each one's text, percent-encoded; the rest of a path keeps its '/'.
template <class Route, std::size_t... Index>
[[nodiscard]] std::string fill_path(typename Route::arguments const &arguments, std::index_sequence<Index...>) {
  constexpr auto &parsed = checked_path<Route::path_value>::parsed;

  std::array<std::string, sizeof...(Index)> const texts{http::detail::percent_encode(
      param_traits<std::tuple_element_t<Index, typename Route::arguments>>::to_text(std::get<Index>(arguments)),
      parsed.params[Index].kind == param_kind::rest_of_path ? "/" : "")...};
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
[[nodiscard]] http::request build_request(typename Route::arguments const &arguments,
                                          credential_map const &credentials) {
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
