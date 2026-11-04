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

#ifndef QUOLL_UI_DETAIL_HOT_HPP
#define QUOLL_UI_DETAIL_HOT_HPP

#include <string_view>

// Hot reload (QUOLL_TLS-like switch, set by the CMake option QUOLL_UI_HOT, for development only): a text or a class
// list written in the source can be replaced while the program runs. So `literal_text`, what tw(), text() and the tags
// take, also records where it was written, which the development server knows from the source it watches.

#if defined(QUOLL_UI_HOT)

#include <algorithm>
#include <concepts>
#include <source_location>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// What was replaced so far, and whom to tell: for the whole program, as what it replaces is the program's source. Only
// in a development build.
struct hot_state {
  std::unordered_map<std::string, std::string> literals;
  std::vector<std::pair<void *, void (*)(void *)>> listeners;
};

[[nodiscard]] inline hot_state &hot() {
  static hot_state state;

  return state;
}

[[nodiscard]] inline std::string hot_key(std::string_view file, unsigned line, unsigned column) {
  return std::string{file} + ':' + std::to_string(line) + ':' + std::to_string(column);
}

/** @brief Replaces the literal written at this place, and shows it. */
inline void hot_replace(std::string_view file, unsigned line, unsigned column, std::string_view value) {
  hot().literals.insert_or_assign(hot_key(file, line, column), std::string{value});

  for (auto const &[owner, changed] : hot().listeners) {
    changed(owner);
  }
}

inline void hot_subscribe(void *owner, void (*changed)(void *)) { hot().listeners.emplace_back(owner, changed); }

inline void hot_unsubscribe(void *owner) {
  std::erase_if(hot().listeners, [owner](auto const &listener) { return listener.first == owner; });
}

// A text from the source: what it says now.
class literal_text {
public:
  // The default argument is evaluated where the conversion takes place: the place of the literal.
  template <std::convertible_to<std::string_view> S>
  literal_text(S const &text, std::source_location where = std::source_location::current()) : value_(text) {
    auto const found = hot().literals.find(hot_key(where.file_name(), where.line(), where.column()));

    if (found != hot().literals.end()) {
      value_ = found->second;
    }
  }

  operator std::string_view() const noexcept { return value_; }

private:
  std::string_view value_;
};

} // namespace quoll::ui::detail

#else

namespace quoll::ui::detail {

using literal_text = std::string_view;

} // namespace quoll::ui::detail

#endif

#endif // QUOLL_UI_DETAIL_HOT_HPP
