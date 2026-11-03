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

#ifndef QUOLL_REST_JSON_CODEC_HPP
#define QUOLL_REST_JSON_CODEC_HPP

#include <quoll/rest/codec.hpp>
#include <quoll/rest/decode_error.hpp>
#include <quoll/rest/detail/json_read_options.hpp>

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace quoll::rest {

/** @brief How a JSON codec treats a body that does not match its type exactly. */
struct json_options {
  /** @brief Skip the keys the type does not have, instead of rejecting the body. */
  bool ignore_unknown_keys = false;
};

/**
 * @brief JSON bodies, read and written by Glaze from the type's own fields.
 *
 * Aggregates need no declaration: Glaze reflects their fields. A `glz::meta<T>` specialization renames fields, maps
 * an enum to names, or describes a type that is not an aggregate. Empty `std::optional` fields are not written.
 *
 * Decoding is strict on what the type requires: every field that is not an `std::optional` (or another nullable type)
 * must be present, a value of the wrong type is an error, and so is anything but whitespace after the value. Unknown
 * keys are rejected unless `Options` ignores them, as a client of a third-party API does (lenient_json_codec). Decoded
 * types are default constructible.
 *
 * Reflected types need external linkage: Glaze reads their field names through an `extern` object, which Clang
 * rejects for a type in an anonymous namespace.
 */
template <json_options Options = json_options{}>
class basic_json_codec {
public:
  static constexpr std::string_view content_type = "application/json";

  /** @throws std::invalid_argument for a value Glaze cannot write, e.g. an enum value without a name. */
  template <class T>
  [[nodiscard]] std::string encode(T const &value) const {
    std::string body;

    if (auto const error = glz::write_json(value, body)) {
      throw std::invalid_argument{"quoll::rest: cannot encode JSON: " + glz::format_error(error)};
    }

    return body;
  }

  /** @brief The value `body` holds, or why it does not match `T`, with the position in the body. */
  template <class T>
    requires std::is_default_constructible_v<T>
  [[nodiscard]] std::expected<T, decode_error> decode(std::string_view body) const {
    T value{};

    if (auto const error = glz::read<read_options>(value, body)) {
      return std::unexpected{decode_error{glz::format_error(error, body)}};
    }

    return value;
  }

private:
  static constexpr auto read_options = detail::make_json_read_options(Options.ignore_unknown_keys);
};

/** @brief JSON codec of Quoll's own APIs: unknown keys are errors. */
using json_codec = basic_json_codec<>;

/** @brief JSON codec for third-party APIs, which add fields over time: unknown keys are skipped. */
using lenient_json_codec = basic_json_codec<json_options{.ignore_unknown_keys = true}>;

static_assert(Codec<json_codec, int>);
static_assert(Codec<lenient_json_codec, int>);

} // namespace quoll::rest

#endif // QUOLL_REST_JSON_CODEC_HPP
