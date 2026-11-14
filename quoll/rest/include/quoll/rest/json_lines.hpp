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

#ifndef QUOLL_REST_JSON_LINES_HPP
#define QUOLL_REST_JSON_LINES_HPP

#include <quoll/rest/codec.hpp>
#include <quoll/rest/decode_error.hpp>
#include <quoll/rest/framing.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/stream.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest {

/**
 * @brief A Framing of a value per line, each encoded by `LineCodec`, JSON by default: newline-delimited JSON, as
 * Docker's events or Kubernetes' watches send them. A CR before the LF is read as part of it, and an empty line
 * skipped.
 */
template <class T, class LineCodec = json_codec>
struct json_lines_framing {
  static_assert(Codec<LineCodec, T>, "quoll::rest::json_lines: the codec cannot encode and decode the type");

  using value_type = T;

  static constexpr std::string_view content_type = "application/x-ndjson";

  [[nodiscard]] std::string encode(T const &value) const {
    auto line = LineCodec{}.encode(value);

    line += '\n';
    return line;
  }

  [[nodiscard]] std::expected<std::optional<framed<T>>, decode_error> decode(std::string_view pending) const {
    std::size_t begin = 0;

    for (;;) {
      auto const end = pending.find('\n', begin);

      if (end == std::string_view::npos) {
        return std::optional<framed<T>>{};
      }

      auto line = pending.substr(begin, end - begin);

      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }

      // An empty line, skipped, its bytes taken with the next value's.
      if (line.empty()) {
        begin = end + 1;
        continue;
      }

      auto value = LineCodec{}.template decode<T>(line);

      if (!value) {
        return std::unexpected{std::move(value.error())};
      }

      return std::optional{framed<T>{std::move(*value), end + 1}};
    }
  }
};

static_assert(Framing<json_lines_framing<int>>);

/** @brief A route's result: a stream of values, one per line of JSON, `rest::get<"/events", rest::json_lines<event>>`.
 */
template <class T, class LineCodec = json_codec>
using json_lines = stream<json_lines_framing<T, LineCodec>>;

} // namespace quoll::rest

#endif // QUOLL_REST_JSON_LINES_HPP
