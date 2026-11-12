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

#ifndef QUOLL_UI_DETAIL_TEXT_VALUE_HPP
#define QUOLL_UI_DETAIL_TEXT_VALUE_HPP

#include <charconv>
#include <cmath>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace quoll::ui::detail {

// What reads from a text and writes as one, as a field of a route or of a form does: a number, a bool, a string.
template <class T>
inline constexpr bool text_value =
    std::same_as<T, std::string> || std::same_as<T, bool> || std::integral<T> || std::floating_point<T>;

// `text` read as a T, or none: a number written whole, `true` or `false`, a string as it is.
template <class T>
  requires text_value<T>
[[nodiscard]] std::optional<T> read_text(std::string_view text) {
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

    // Not a NaN, which no value equals, nor an infinity: a number written.
    if constexpr (std::floating_point<T>) {
      if (!std::isfinite(value)) {
        return std::nullopt;
      }
    }

    return value;
  }
}

// `value` written as text, the shortest that reads back as it, none when it does not fit.
template <class T>
  requires text_value<T>
[[nodiscard]] std::optional<std::string> write_text(T const &value) {
  if constexpr (std::same_as<T, std::string>) {
    return value;
  } else if constexpr (std::same_as<T, bool>) {
    return std::string{value ? "true" : "false"};
  } else {
    char buffer[64];
    auto const [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);

    return error == std::errc{} ? std::optional<std::string>{std::string{buffer, end}} : std::nullopt;
  }
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_TEXT_VALUE_HPP
