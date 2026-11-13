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

#ifndef QUOLL_UI_DETAIL_ROUTING_ROUTE_FIELD_HPP
#define QUOLL_UI_DETAIL_ROUTING_ROUTE_FIELD_HPP

#include <quoll/http/fixed_string.hpp>
#include <quoll/ui/detail/text_value.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

template <class T>
inline constexpr bool is_optional = false;

template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;

// What a field of a page's props may be, to come from its route's path or query: a number, a bool, a string, or an
// optional of one.
template <class T>
inline constexpr bool route_field = text_value<T>;

template <class T>
inline constexpr bool route_field<std::optional<T>> = text_value<T>;

// Sets `field` from `text`: false when it is no such value, `field` left as it was.
template <class T>
[[nodiscard]] bool parse_route_field(std::string_view text, T &field) {
  if constexpr (is_optional<T>) {
    auto value = read_text<typename T::value_type>(text);

    if (!value) {
      return false;
    }

    field = std::move(*value);
  } else {
    auto value = read_text<T>(text);

    if (!value) {
      return false;
    }

    field = std::move(*value);
  }

  return true;
}

// `value` written as text, none for an empty optional.
template <class T>
[[nodiscard]] std::optional<std::string> format_route_field(T const &value) {
  if constexpr (is_optional<T>) {
    return value ? write_text(*value) : std::nullopt;
  } else {
    return write_text(value);
  }
}

// What a member pointer names: the struct it is a field of, and the field's type.
template <class M>
struct member_traits {
  static constexpr bool valid = false;
};

template <class C, class F>
struct member_traits<F C::*> {
  static constexpr bool valid = true;
  using owner = C;
  using field = F;
};

// A field of a page's props that the address fills by a name: the query's, or a parameter of the path matched,
// that of the route or of one under it.
template <http::fixed_string Name, auto Member, bool InPath>
struct named_field {
  static constexpr auto name = Name;
  static constexpr auto member = Member;
  static constexpr bool in_path = InPath;
};

template <class F>
inline constexpr bool is_named_field = false;

template <http::fixed_string Name, auto Member, bool InPath>
inline constexpr bool is_named_field<named_field<Name, Member, InPath>> = true;

// A field a route names, `&props::id` from its path's {parameters} in order, or by name, `from_query` and `from_path`
// (see route): its member pointer.
template <auto Field>
[[nodiscard]] consteval auto member_of() {
  if constexpr (is_named_field<std::remove_cvref_t<decltype(Field)>>) {
    return std::remove_cvref_t<decltype(Field)>::member;
  } else {
    return Field;
  }
}

// The fields a route names, in the order written: those of its path, in the order of its {parameters}, and those of
// its query, by name.
template <auto... Fields>
struct page_fields {};

// The field at `Index` of those a route names.
template <std::size_t Index, auto... Fields>
inline constexpr auto field_at = std::get<Index>(std::tuple{Fields...});

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_ROUTING_ROUTE_FIELD_HPP
