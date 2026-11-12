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

#include <glaze/core/reflect.hpp>

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
inline constexpr bool route_value =
    std::same_as<T, std::string> || std::same_as<T, bool> || std::integral<T> || std::floating_point<T>;

template <class T>
inline constexpr bool route_field = route_value<T>;

template <class T>
inline constexpr bool route_field<std::optional<T>> = route_value<T>;

// `text` read as a T, or none: a number written whole, `true` or `false`, a string as it is.
template <class T>
[[nodiscard]] std::optional<T> parse_route_value(std::string_view text) {
  if constexpr (std::same_as<T, std::string>) {
    return std::string{text};
  } else if constexpr (std::same_as<T, bool>) {
    if (text == "true") {
      return true;
    }

    if (text == "false") {
      return false;
    }

    return std::nullopt;
  } else {
    T value{};
    auto const *const end = text.data() + text.size();
    auto const [stop, error] = std::from_chars(text.data(), end, value);

    if (error != std::errc{} || stop != end) {
      return std::nullopt;
    }

    // Not a NaN, which no value equals, nor an infinity: what an address names is a number.
    if constexpr (std::floating_point<T>) {
      if (!std::isfinite(value)) {
        return std::nullopt;
      }
    }

    return value;
  }
}

// Sets `field` from `text`: false when it is no such value, `field` left as it was.
template <class T>
[[nodiscard]] bool parse_route_field(std::string_view text, T &field) {
  if constexpr (is_optional<T>) {
    auto value = parse_route_value<typename T::value_type>(text);

    if (!value) {
      return false;
    }

    field = std::move(*value);
  } else {
    auto value = parse_route_value<T>(text);

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
    return value ? format_route_field(*value) : std::nullopt;
  } else if constexpr (std::same_as<T, std::string>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    return std::string{value ? "true" : "false"};
  } else {
    char buffer[64];
    auto const [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);

    return error == std::errc{} ? std::optional<std::string>{std::string{buffer, end}} : std::nullopt;
  }
}

// Calls `visit(name, field)` for each field of the props `value`, with its name.
template <class P, class Visitor>
void for_each_route_field(P &value, Visitor &&visit) {
  using type = std::remove_const_t<P>;

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (visit(glz::reflect<type>::keys[Index], glz::get<Index>(glz::to_tie(value))), ...);
  }(std::make_index_sequence<glz::reflect<type>::size>{});
}

// Calls `visit(name, field, other)` for each field of `value` and the same of `other`, two props of one type.
template <class P, class Visitor>
void for_each_route_field_pair(P const &value, P const &other, Visitor &&visit) {
  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (visit(glz::reflect<P>::keys[Index], glz::get<Index>(glz::to_tie(value)), glz::get<Index>(glz::to_tie(other))),
     ...);
  }(std::make_index_sequence<glz::reflect<P>::size>{});
}

// Whether the props P have a field named `name`, and whether every field may come from a route.
template <class P>
[[nodiscard]] constexpr bool has_route_field(std::string_view name) {
  for (auto const key : glz::reflect<P>::keys) {
    if (key == name) {
      return true;
    }
  }

  return false;
}

template <class P>
[[nodiscard]] consteval bool route_fields_only() {
  return []<std::size_t... Index>(std::index_sequence<Index...>) {
    return (route_field<std::remove_cvref_t<decltype(glz::get<Index>(glz::to_tie(std::declval<P &>())))>> && ...);
  }(std::make_index_sequence<glz::reflect<P>::size>{});
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_ROUTING_ROUTE_FIELD_HPP
