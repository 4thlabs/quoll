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

#include <quoll/rest/codec.hpp>
#include <quoll/rest/decode_error.hpp>
#include <quoll/rest/detail/arguments.hpp>
#include <quoll/rest/detail/client_request.hpp>
#include <quoll/rest/detail/fields_of.hpp>
#include <quoll/rest/detail/query_string.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::rest {

/**
 * @brief Bodies of a form, `application/x-www-form-urlencoded`: the fields of an aggregate by name, each converted as
 * a query parameter is (see param_traits). A vector repeats its name, an optional without a value is left out, and
 * anything else is required. What a browser's form sends, and many APIs take (an OAuth token endpoint).
 *
 * @code
 * struct login {
 *   std::string username;
 *   std::string password;
 * };
 *
 * using sign_in = rest::endpoint<"sign_in", verb::post, "/login", rest::body<login, rest::form_codec>>;
 * @endcode
 */
class form_codec {
public:
  static constexpr std::string_view content_type = "application/x-www-form-urlencoded";

  template <class T>
    requires std::is_aggregate_v<T>
  [[nodiscard]] std::string encode(T const &value) const {
    std::string body;

    detail::for_each_field(value, [&](std::string_view name, auto const &field) {
      detail::for_each_text(field, [&](std::string const &text) { detail::append_query(body, name, text); });
    });

    return body;
  }

  /** @brief The form `body` holds, or a decode_error for one that is not URL-encoded or misses a required field. */
  template <class T>
    requires std::is_aggregate_v<T>
  [[nodiscard]] std::expected<T, decode_error> decode(std::string_view body) const {
    auto const entries = detail::parse_query(body);

    if (!entries) {
      return std::unexpected{decode_error{"a form that is not URL-encoded"}};
    }

    T value{};
    std::optional<decode_error> error;

    detail::for_each_field(value, [&](std::string_view name, auto &field) {
      if (error) {
        return;
      }

      auto decoded = detail::from_texts<std::remove_cvref_t<decltype(field)>>("form field", name,
                                                                              detail::query_texts(*entries, name));

      if (!decoded) {
        error = decode_error{std::move(decoded.error().detail)};
        return;
      }

      field = std::move(*decoded);
    });

    if (error) {
      return std::unexpected{std::move(*error)};
    }

    return value;
  }
};

} // namespace quoll::rest

#endif // QUOLL_REST_FORM_CODEC_HPP
