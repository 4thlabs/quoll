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

#ifndef QUOLL_UI_DETAIL_VIEW_HOT_HPP
#define QUOLL_UI_DETAIL_VIEW_HOT_HPP

#include <optional>
#include <string_view>

#if defined(QUOLL_UI_HOT)
#include <concepts>
#include <source_location>
#endif

// Hot reload (QUOLL_TLS-like switch, set by the CMake option QUOLL_UI_HOT, for development only): a text or a class
// list written in the source can be replaced while the program runs. So `literal_text`, what tw(), text() and the tags
// take, also records where it was written, which the development server knows from the source it watches.
//
// The functions are compiled once and are only linked into a program using them. The switch only changes what the
// program's own sources compile: the library is built without it. So no function of the library's sources takes a
// `literal_text`, whose type differs on each side, and would be undefined in a program built with the switch: what
// takes one is inline in a header, as tw(), text(), the tags and link_to() are, and forwards a `std::string_view` or
// an element to the library.

namespace quoll::ui::detail {

/** @brief Replaces the literal written at this place, and shows it. */
void hot_replace(std::string_view file, unsigned line, unsigned column, std::string_view value);

/** @brief What replaced the literal written at this place, valid until it is replaced again. */
[[nodiscard]] std::optional<std::string_view> hot_find(std::string_view file, unsigned line, unsigned column);

void hot_subscribe(void *owner, void (*changed)(void *));
void hot_unsubscribe(void *owner);

#if defined(QUOLL_UI_HOT)

// A text from the source: what it says now.
class literal_text {
public:
  // The default argument is evaluated where the conversion takes place: the place of the literal.
  template <std::convertible_to<std::string_view> S>
  literal_text(S const &text, std::source_location where = std::source_location::current()) : value_(text) {
    if (auto const found = hot_find(where.file_name(), where.line(), where.column())) {
      value_ = *found;
    }
  }

  operator std::string_view() const noexcept { return value_; }

private:
  std::string_view value_;
};

#else

using literal_text = std::string_view;

#endif

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_VIEW_HOT_HPP
