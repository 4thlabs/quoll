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

#ifndef QUOLL_HTTP_DETAIL_PLAIN_SEGMENT_HPP
#define QUOLL_HTTP_DETAIL_PLAIN_SEGMENT_HPP

#include <cstddef>
#include <string_view>

namespace quoll::http::detail {

// What decoded text comes to, a character at a time: no NUL, which C APIs would stop at, and no `.` or `..` part,
// whether alone or behind a slash the encoding hides (`..%2Fetc`, `a%5C..`), which a handler joining it to a directory
// would follow. One rule for the router, a base URL, a typed client's path parameters, and at compile time for a route.
class plain_text {
public:
  constexpr void feed(char const character) noexcept {
    if (character == '\0') {
      plain_ = false;
    } else if (character == '/' || character == '\\') {
      plain_ = plain_ && !is_dot_part();
      size_ = 0;
      dots_ = 0;
    } else {
      ++size_;
      dots_ += character == '.' ? 1 : 0;
    }
  }

  [[nodiscard]] constexpr bool plain() const noexcept { return plain_ && !is_dot_part(); }

private:
  [[nodiscard]] constexpr bool is_dot_part() const noexcept { return size_ == dots_ && (size_ == 1 || size_ == 2); }

  std::size_t size_ = 0;
  std::size_t dots_ = 0;
  bool plain_ = true;
};

[[nodiscard]] constexpr bool plain_segment(std::string_view decoded) noexcept {
  plain_text text;

  for (auto const character : decoded) {
    text.feed(character);
  }

  return text.plain();
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_PLAIN_SEGMENT_HPP
