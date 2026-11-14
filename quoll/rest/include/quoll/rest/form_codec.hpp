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

#ifndef QUOLL_REST_FORM_CODEC_HPP
#define QUOLL_REST_FORM_CODEC_HPP

#include <quoll/http/detail/query_string.hpp>
#include <quoll/rest/codec.hpp>
#include <quoll/rest/decode_error.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/fields_by_name.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest {

/**
 * @brief Bodies of a form, `application/x-www-form-urlencoded`: the fields of a struct by name, each converted as
 * a query parameter is (see param_traits). A vector repeats its name, an optional without a value is left out, and
 * anything else is required, once. What a browser's form sends, and many APIs take (an OAuth token endpoint).
 *
 * The struct is an aggregate of 64 fields at most, with no base class, each field with a default value and none a
 * reference, its fields named as they are declared or as its `rest_names` give (see names). A form
 * of more than max_fields `name=value` pairs is refused before it is parsed, as a body may be far larger than a query.
 *
 * @code
 * struct login {
 *   std::string username;
 *   std::string password;
 * };
 *
 * using sign_in = rest::post<"/login", token, rest::form<login>>;
 * @endcode
 */
class form_codec {
public:
  static constexpr std::string_view content_type = "application/x-www-form-urlencoded";

  /** @brief The most `name=value` pairs a form decodes. */
  static constexpr std::size_t max_fields = 1000;

  /** @throws std::invalid_argument for a field holding a NUL, which a server refuses. */
  template <detail::FieldsByName T>
  [[nodiscard]] std::string encode(T const &value) const {
    std::string body;

    detail::append_fields(body, value, "quoll::rest::form_codec: field");
    return body;
  }

  /**
   * @brief The form `body` holds, or a decode_error for one that has too many fields, is not URL-encoded, holds a
   * NUL, misses a required field or repeats one that is not a vector.
   */
  template <detail::FieldsByName T>
  [[nodiscard]] std::expected<T, decode_error> decode(std::string_view body) const {
    if (static_cast<std::size_t>(std::ranges::count(body, '&')) >= max_fields) {
      return std::unexpected{decode_error{"a form of more than " + std::to_string(max_fields) + " fields"}};
    }

    auto const entries = http::detail::parse_query(body);

    if (!entries) {
      return std::unexpected{decode_error{"a form that is not URL-encoded, or holds a NUL"}};
    }

    auto decoded = detail::decode_fields<T>("form field", *entries);

    if (!decoded) {
      return std::unexpected{decode_error{std::move(decoded.error().detail)}};
    }

    return std::move(*decoded);
  }
};

namespace detail {

// A struct to check form_codec against rest::Codec with.
struct form_codec_model {
  std::string name;
};

} // namespace detail

static_assert(Codec<form_codec, detail::form_codec_model>);

} // namespace quoll::rest

#endif // QUOLL_REST_FORM_CODEC_HPP
