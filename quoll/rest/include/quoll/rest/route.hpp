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

#ifndef QUOLL_REST_ROUTE_HPP
#define QUOLL_REST_ROUTE_HPP

#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest/body.hpp>
#include <quoll/rest/bytes_codec.hpp>
#include <quoll/rest/detail/category.hpp>
#include <quoll/rest/detail/expression.hpp>
#include <quoll/rest/detail/operation_concept.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/events.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/form_codec.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/parameter.hpp>
#include <quoll/rest/query.hpp>
#include <quoll/rest/returns.hpp>
#include <quoll/rest/security.hpp>
#include <quoll/rest/text_codec.hpp>

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest {

/** @brief A result answered `201 Created`: `rest::post<"/notes", rest::created<note>, new_note>`. */
template <class T>
struct created {};

/** @brief A result answered `202 Accepted`. */
template <class T>
struct accepted {};

/** @brief A body or a result of plain text, `text/plain`, as a std::string. */
struct text {};

/** @brief A body or a result of bytes, `application/octet-stream`, as a std::vector<std::byte>. */
struct bytes {};

/** @brief A body or a result in JSON, what a type alone already is: to say it. */
template <class T>
struct json {};

/** @brief A result in JSON read leniently: the fields its type does not know are skipped, as an API adds some. */
template <class T>
struct lenient_json {};

/** @brief A body of a form, `application/x-www-form-urlencoded`: the fields of `T` by name (see form_codec). */
template <class T>
struct form {};

/** @brief The name of a route's operation, OpenAPI's operationId: `rest::operation_id<"notes.get">`. */
template <fixed_string Name>
struct operation_id {
  static_assert(!Name.view().empty(), "quoll::rest::operation_id: the name is empty");

  static constexpr std::string_view value = Name.view();
};

namespace detail {

// What a value of a body or a result is, and its codec.
template <class T>
struct payload_of {
  using type = T;
  using codec = json_codec;
};

template <>
struct payload_of<text> {
  using type = std::string;
  using codec = text_codec;
};

template <>
struct payload_of<bytes> {
  using type = std::vector<std::byte>;
  using codec = bytes_codec;
};

template <class T>
struct payload_of<json<T>> {
  using type = T;
  using codec = json_codec;
};

template <class T>
struct payload_of<lenient_json<T>> {
  using type = T;
  using codec = lenient_json_codec;
};

template <class T>
struct payload_of<form<T>> {
  using type = T;
  using codec = form_codec;
};

// The parts a route's result gives: none for void, an answer of 204; events<> as they are; a returns<> otherwise.
template <class Result>
struct result_parts {
  using type =
      type_list<returns<typename payload_of<Result>::type, http::status::ok, typename payload_of<Result>::codec>>;
};

template <>
struct result_parts<void> {
  using type = type_list<>;
};

template <class T>
struct result_parts<created<T>> {
  using type = type_list<returns<typename payload_of<T>::type, http::status::created, typename payload_of<T>::codec>>;
};

template <class T>
struct result_parts<accepted<T>> {
  using type = type_list<returns<typename payload_of<T>::type, http::status::accepted, typename payload_of<T>::codec>>;
};

template <class T, class Codec>
struct result_parts<events<T, Codec>> {
  using type = type_list<events<T, Codec>>;
};

// What of a route's arguments is no type of a value: its operation's name, its security.
template <class T>
struct is_route_option : std::false_type {};

template <fixed_string Name>
struct is_route_option<operation_id<Name>> : std::true_type {};

template <class... Schemes>
struct is_route_option<security<Schemes...>> : std::true_type {};

template <class T>
struct is_value_type : std::bool_constant<!is_route_option<T>::value> {};

template <class T>
struct is_operation_id : std::false_type {};

template <fixed_string Name>
struct is_operation_id<operation_id<Name>> : std::true_type {};

template <class Options>
struct operation_name {
  static constexpr std::string_view value{};
};

template <class First, class... Rest>
struct operation_name<type_list<First, Rest...>> {
  static constexpr std::string_view value = First::value;
};

// The `Count` types of `List` from `Begin`.
template <class List, std::size_t Begin, std::size_t Count, class Indices = std::make_index_sequence<Count>>
struct slice_types;

template <class... Ts, std::size_t Begin, std::size_t Count, std::size_t... Index>
struct slice_types<type_list<Ts...>, Begin, Count, std::index_sequence<Index...>> {
  using type = type_list<std::tuple_element_t<Begin + Index, std::tuple<Ts...>>...>;
};

template <class List, std::size_t Begin>
using drop_types = slice_types<List, Begin, (List::size > Begin ? List::size - Begin : 0)>;

// The query<> parts of a route's expression, its `Index`th key with the type its arguments give.
template <fixed_string Expr, class Types, class Indices = std::make_index_sequence<Types::size>>
struct expression_query_parts;

template <fixed_string Expr, class... Types, std::size_t... Index>
struct expression_query_parts<Expr, type_list<Types...>, std::index_sequence<Index...>> {
  using type = type_list<query<query_key<Expr, Index>, Types>...>;
};

// The part a body type gives: body<> of its payload, with its codec.
template <class T>
struct body_parts {
  using type = type_list<body<typename payload_of<T>::type, typename payload_of<T>::codec>>;
};

template <class T>
struct is_tail_part : std::bool_constant<Part<T>> {};

template <class T>
struct is_body_type : std::bool_constant<!Part<T>> {};

template <class Bodies>
struct body_parts_of;

template <>
struct body_parts_of<type_list<>> {
  using type = type_list<>;
};

template <class Body>
struct body_parts_of<type_list<Body>> {
  using type = typename body_parts<Body>::type;
};

// Whether the last path parameter, the rest of the path when there is one, is a std::string.
template <bool RestOfPath, class PathTypes>
[[nodiscard]] consteval bool rest_of_path_is_text() {
  if constexpr (RestOfPath && PathTypes::size > 0) {
    return std::same_as<std::tuple_element_t<PathTypes::size - 1, as_tuple_t<PathTypes>>, std::string>;
  } else {
    return true;
  }
}

// Whether the body, if the tail has one, is its last type.
template <class Tail>
[[nodiscard]] consteval bool body_comes_last() {
  if constexpr (Tail::size == 0) {
    return true;
  } else {
    return filter_t<is_body_type, Tail>::size == 0 ||
           is_body_type<std::tuple_element_t<Tail::size - 1, as_tuple_t<Tail>>>::value;
  }
}

// Whether each type is a path parameter's: read from text.
template <class... Types>
[[nodiscard]] consteval bool all_query_values(type_list<Types...> /*types*/) {
  return (is_parameter_value<Types>::value && ...);
}

template <class... Types>
[[nodiscard]] consteval bool all_parameters(type_list<Types...> /*types*/) {
  return (Parameter<Types> && ...);
}

// A route of `Method` at `Expr`, answering `Result`, taking `Args`: its parameters' types, its headers, its body. What
// the REST layer serves and calls, as endpoint<> was: each route is its own type, as it is a struct of its own.
template <http::verb Method, fixed_string Expr, class Result, class... Args>
struct operation : operation_tag {
  using expression_type = expression<Expr>;

  static_assert(
      expression_type::error() == http::detail::template_error::none,
      "quoll::rest: a route's expression is a path and a query, `/notes/{id}?fields={fields}`, valid as the router "
      "reads it"
  );
  static_assert(
      !expression_type::typed_in_path(),
      "quoll::rest: a route says the types of its parameters after its expression, not in it: "
      "`rest::get<\"/notes/{id}\", note, std::int64_t>`; only {name:path} says one"
  );
  static_assert(
      !expression_type::has_fixed_value(), "quoll::rest: a fixed value in a route's query comes later (#176)"
  );
  static_assert(
      expression_type::plain_keys(), "quoll::rest: a key of a route's query is written plain, with no escape nor '+'"
  );

  using values = filter_t<is_value_type, type_list<Args...>>;
  using options = filter_t<is_route_option, type_list<Args...>>;

  static constexpr std::size_t path_count = expression_type::path_count();
  static constexpr std::size_t query_count = expression_type::query_count();

  static_assert(
      values::size >= path_count + query_count,
      "quoll::rest: a route has a type for each {parameter} of its expression, in order, after its result"
  );

  using path_types = typename slice_types<values, 0, path_count>::type;
  using query_types = typename slice_types<values, path_count, query_count>::type;
  using tail = typename drop_types<values, path_count + query_count>::type;
  using bodies = filter_t<is_body_type, tail>;

  static_assert(bodies::size <= 1, "quoll::rest: a route has one body at most, its last type");
  static_assert(
      body_comes_last<tail>(), "quoll::rest: a route's body is its last type, after its parameters and headers"
  );
  static_assert(
      all_parameters(path_types{}),
      "quoll::rest: a path parameter's type is read from text (a Parameter), never a std::optional nor a std::vector, "
      "which only a query parameter may be"
  );
  static_assert(
      all_query_values(query_types{}), "quoll::rest: a query parameter's type is read from text (a Parameter), or a "
                                       "std::optional or a std::vector of one"
  );
  static_assert(
      Method != http::verb::head || std::is_void_v<Result>,
      "quoll::rest: a response to HEAD has no body: its result is void"
  );
  static_assert(filter_t<is_operation_id, options>::size <= 1, "quoll::rest: a route has one operation_id<> at most");
  static_assert(
      rest_of_path_is_text<expression_type::rest_of_path(), path_types>(),
      "quoll::rest: a {name:path} parameter, the rest of the path, is a std::string"
  );
  using contract_category = endpoint_category;
  using parts = concat_t<
      typename expression_query_parts<Expr, query_types>::type, filter_t<is_tail_part, tail>,
      typename body_parts_of<bodies>::type, typename result_parts<Result>::type,
      filter_t<is_security_part, filter_t<is_tail_part, options>>>;

  // On its parts, so that a body<> written as a part counts too; a group's, inherited, are checked by its route.
  static_assert(
      count_bodies(parts{}) == 0 || Method == http::verb::post || Method == http::verb::put ||
          Method == http::verb::patch,
      "quoll::rest: only a post, a put or a patch has a body"
  );

  static constexpr http::verb method = Method;
  static constexpr auto path_value = expression_type::path;
  static constexpr std::string_view path = path_value.view();
  // Its name, given by operation_id<>, or none: the route's method and whole path then.
  static constexpr std::string_view name = operation_name<filter_t<is_operation_id, options>>::value;
};

} // namespace detail

/**
 * @brief A route answering GET: its expression, its path and query below the api's prefix, its result, then a type
 * for each of its `{parameters}`, in order, then its headers.
 *
 * @code
 * using get = rest::get<"/notes/{id}?fields={fields}", note, std::int64_t, std::optional<std::string>>;
 * @endcode
 *
 * Its result is answered 200, or 204 for `void`; `created<T>` and `accepted<T>` say 201 and 202; `text`, `bytes`,
 * `json<T>` and `lenient_json<T>` say its format, JSON by default. A query parameter is a std::optional to be missing
 * and a std::vector to repeat. `header<"X-Id", T>` adds a header, `security<...>` a scheme, `operation_id<"...">` a
 * name. Each route is its own type, which a service names (see service).
 */
template <fixed_string Expr, class Result, class... Args>
struct get : detail::operation<http::verb::get, Expr, Result, Args...> {};

/** @brief A route answering POST, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct post : detail::operation<http::verb::post, Expr, Result, Args...> {};

/** @brief A route answering PUT, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct put : detail::operation<http::verb::put, Expr, Result, Args...> {};

/** @brief A route answering PATCH, its body its last type: see get. */
template <fixed_string Expr, class Result, class... Args>
struct patch : detail::operation<http::verb::patch, Expr, Result, Args...> {};

/** @brief A route answering DELETE (`delete` is a keyword): see get. */
template <fixed_string Expr, class Result, class... Args>
struct del : detail::operation<http::verb::delete_, Expr, Result, Args...> {};

/** @brief A route answering HEAD: see get. */
template <fixed_string Expr, class Result, class... Args>
struct head : detail::operation<http::verb::head, Expr, Result, Args...> {};

/** @brief A route answering OPTIONS: see get. */
template <fixed_string Expr, class Result, class... Args>
struct options : detail::operation<http::verb::options, Expr, Result, Args...> {};

} // namespace quoll::rest

#endif // QUOLL_REST_ROUTE_HPP
