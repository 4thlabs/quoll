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

#ifndef QUOLL_REST_DETAIL_SCHEME_TRAITS_HPP
#define QUOLL_REST_DETAIL_SCHEME_TRAITS_HPP

#include <quoll/http/fields.hpp>
#include <quoll/rest/detail/join_strings.hpp>
#include <quoll/http/detail/query_string.hpp>
#include <quoll/rest/detail/single_field.hpp>
#include <quoll/rest/fixed_string.hpp>
#include <quoll/rest/security.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::rest::detail {

// A credential the server reads back as it is, at the call: empty is none to it, parsers trim surrounding whitespace,
// and a control character cannot go out.
void check_credential_text(std::string_view text);

// A credential in a query goes encoded, so that it reads back as it is: only empty is none to the server.
void check_credential_in_query(std::string_view text);

// Everything about a security scheme, for the server and the client alike:
// - `key`: its key in a client's credentials, known at compile time;
// - `find(fields, query)`: the credential a request carries, if any (an empty one is none);
// - `challenge()`: the WWW-Authenticate value of a 401, which RFC 9110 requires (API keys have no registered scheme:
//   `ApiKey` tells the client where the key goes);
// - `add(value, fields, query)`: sends a credential with a request.
template <class Scheme>
struct scheme_traits;

template <>
struct scheme_traits<bearer_auth> {
  static constexpr auto key = fixed_string{"bearer"};

  [[nodiscard]] static std::optional<std::string>
  find(http::fields const &fields, std::vector<http::detail::query_entry> const &query);

  [[nodiscard]] static std::string challenge();

  static void add(std::string_view value, http::fields &fields, std::string &query);
};

template <fixed_string Header>
struct scheme_traits<api_key<Header>> {
  static constexpr auto key = join_strings<"header:", Header>();

  [[nodiscard]] static std::optional<std::string> find(http::fields const &fields, std::vector<http::detail::query_entry> const &) {
    auto const value = single_field(fields, Header.view());

    return value && !value->empty() ? std::optional<std::string>{*value} : std::nullopt;
  }

  [[nodiscard]] static std::string challenge() { return "ApiKey header=\"" + std::string{Header.view()} + "\""; }

  static void add(std::string_view value, http::fields &fields, std::string &) {
    check_credential_text(value);
    fields.add(Header.view(), value);
  }
};

template <fixed_string Param>
struct scheme_traits<api_key_query<Param>> {
  static constexpr auto key = join_strings<"query:", Param>();

  [[nodiscard]] static std::optional<std::string> find(http::fields const &, std::vector<http::detail::query_entry> const &query) {
    auto const texts = http::detail::query_texts(query, Param.view());

    return texts.size() != 1 || texts.front().empty() ? std::nullopt : std::optional<std::string>{texts.front()};
  }

  [[nodiscard]] static std::string challenge() { return "ApiKey query=\"" + std::string{Param.view()} + "\""; }

  static void add(std::string_view value, http::fields &, std::string &query) {
    check_credential_in_query(value);
    http::detail::append_query(query, Param.view(), value);
  }
};

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_SCHEME_TRAITS_HPP
