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

#ifndef QUOLL_REST_PARAM_TRAITS_HPP
#define QUOLL_REST_PARAM_TRAITS_HPP

#include <quoll/rest/detail/number.hpp>

#include <glaze/core/reflect.hpp>
#include <glaze/json/read.hpp>

#include <algorithm>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace quoll::rest {

/**
 * @brief Converts the values of path, query and header parameters to and from their text.
 *
 * `to_text(value)` returns the text to send; `from_text(text)` returns the value, or nothing for text that is not one
 * (the server answers 400). Specialized for integers, floating-point numbers, `bool` (`true` / `false`),
 * `std::string`, and enums described by `glz::meta` (by name). Specialize it for other types.
 */
template <class T>
struct param_traits;

/**
 * @brief Integers in decimal, without sign for unsigned types and without a leading `+`. Not `bool`, nor the
 * character types that std::from_chars does not read.
 */
template <std::integral T>
  requires(!detail::is_any_of<T, bool, wchar_t, char8_t, char16_t, char32_t>)
struct param_traits<T> {
  [[nodiscard]] static std::string to_text(T value) { return detail::format_number(value); }

  [[nodiscard]] static std::optional<T> from_text(std::string_view text) noexcept {
    return detail::parse_number<T>(text);
  }
};

/** @brief Finite floating-point numbers, in their shortest form that reads back to the same value; not `nan` or `inf`.
 */
template <std::floating_point T>
struct param_traits<T> {
  [[nodiscard]] static std::string to_text(T value) { return detail::format_number(value); }

  [[nodiscard]] static std::optional<T> from_text(std::string_view text) noexcept {
    return detail::parse_number<T>(text);
  }
};

/** @brief `true` and `false`, nothing else. */
template <>
struct param_traits<bool> {
  [[nodiscard]] static std::string to_text(bool value) { return value ? "true" : "false"; }

  [[nodiscard]] static std::optional<bool> from_text(std::string_view text) noexcept {
    if (text == "true") {
      return true;
    }

    if (text == "false") {
      return false;
    }

    return std::nullopt;
  }
};

/** @brief Any text, as it is. */
template <>
struct param_traits<std::string> {
  [[nodiscard]] static std::string to_text(std::string const &value) { return value; }

  [[nodiscard]] static std::optional<std::string> from_text(std::string_view text) { return std::string{text}; }
};

/** @brief Enums by the names their `glz::meta` gives them. */
template <class T>
  requires std::is_enum_v<T> && glz::glaze_t<T>
struct param_traits<T> {
  /** @throws std::invalid_argument for a value without a name. */
  [[nodiscard]] static std::string to_text(T value) {
    auto const name = glz::get_enum_name(value);

    if (name.empty()) {
      throw std::invalid_argument{"quoll::rest: enum value without a name"};
    }

    return std::string{name};
  }

  [[nodiscard]] static std::optional<T> from_text(std::string_view text) {
    // Read as the JSON string of the name: names never hold a quote, a backslash or a control character.
    if (text.find_first_of("\"\\") != std::string_view::npos ||
        std::ranges::any_of(text, [](char character) { return static_cast<unsigned char>(character) < 0x20; })) {
      return std::nullopt;
    }

    T value{};
    std::string const quoted = '"' + std::string{text} + '"';

    if (glz::read_json(value, quoted)) {
      return std::nullopt;
    }

    return value;
  }
};

} // namespace quoll::rest

#endif // QUOLL_REST_PARAM_TRAITS_HPP
