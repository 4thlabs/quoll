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

#include <quoll/http/detail/utf8.hpp>

#include <cstddef>

namespace quoll::http::detail {

namespace {

constexpr std::string_view replacement = "\xEF\xBF\xBD";

// The length of the valid sequence at `position`, or of its maximal subpart, negated, when it is not one.
[[nodiscard]] std::ptrdiff_t sequence_at(std::string_view text, std::size_t position) noexcept {
  auto const lead = static_cast<unsigned char>(text[position]);

  if (lead < 0x80U) {
    return 1;
  }

  std::size_t continuations = 0;
  unsigned char lower = 0x80U;
  unsigned char upper = 0xBFU;

  if (lead >= 0xC2U && lead <= 0xDFU) {
    continuations = 1;
  } else if (lead >= 0xE0U && lead <= 0xEFU) {
    continuations = 2;
    lower = lead == 0xE0U ? 0xA0U : lower;
    upper = lead == 0xEDU ? 0x9FU : upper;
  } else if (lead >= 0xF0U && lead <= 0xF4U) {
    continuations = 3;
    lower = lead == 0xF0U ? 0x90U : lower;
    upper = lead == 0xF4U ? 0x8FU : upper;
  } else {
    return -1;
  }

  std::size_t taken = 1;

  for (; taken <= continuations && position + taken < text.size(); ++taken) {
    auto const next = static_cast<unsigned char>(text[position + taken]);

    if (next < lower || next > upper) {
      break;
    }

    lower = 0x80U;
    upper = 0xBFU;
  }

  auto const length = static_cast<std::ptrdiff_t>(taken);

  return taken == continuations + 1 ? length : -length;
}

} // namespace

bool is_utf8(std::string_view text) noexcept {
  for (std::size_t position = 0; position < text.size();) {
    auto const length = sequence_at(text, position);

    if (length < 0) {
      return false;
    }

    position += static_cast<std::size_t>(length);
  }

  return true;
}

void append_repaired_utf8(std::string &out, std::string_view text) {
  for (std::size_t position = 0; position < text.size();) {
    auto const length = sequence_at(text, position);

    if (length > 0) {
      out.append(text.substr(position, static_cast<std::size_t>(length)));
      position += static_cast<std::size_t>(length);
    } else {
      out.append(replacement);
      position += static_cast<std::size_t>(-length);
    }
  }
}

} // namespace quoll::http::detail
