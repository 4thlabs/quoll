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

#ifndef QUOLL_HTTP_DETAIL_NUMBER_HPP
#define QUOLL_HTTP_DETAIL_NUMBER_HPP

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace quoll::http::detail {

template <class T, class... Candidates>
inline constexpr bool is_any_of = (std::same_as<T, Candidates> || ...);

// The whole text must be the number: no sign for unsigned types, no leading `+`, no trailing characters. Floating
// point values must be finite: from_chars also reads `nan` and `inf`, which no parameter means.
template <class T>
[[nodiscard]] std::optional<T> parse_number(std::string_view text) noexcept {
  T value{};
  auto const last = text.data() + text.size();
  auto const [end, error] = std::from_chars(text.data(), last, value);

  if (error != std::errc{} || end != last) {
    return std::nullopt;
  }

  if constexpr (std::floating_point<T>) {
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
  }

  return value;
}

// `true` and `false`, nothing else.
[[nodiscard]] constexpr std::optional<bool> parse_bool(std::string_view text) noexcept {
  if (text == "true") {
    return true;
  }

  if (text == "false") {
    return false;
  }

  return std::nullopt;
}

template <class T>
[[nodiscard]] std::string format_number(T value) {
  // Enough for any integer up to 128 bits, and for the shortest round-trip form of any standard floating-point type.
  std::array<char, 64> buffer{};
  auto const written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

  return {buffer.data(), written.ptr};
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_NUMBER_HPP
