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

#ifndef QUOLL_REST_SECURITY_HPP
#define QUOLL_REST_SECURITY_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <concepts>
#include <string_view>

namespace quoll::rest {

/** @brief Security scheme: a key sent in the request header field `Header`. */
template <fixed_string Header>
struct api_key {
  static_assert(http::detail::is_token(Header.view()), "quoll::rest::api_key: the name is not a valid field name");

  using scheme_category = detail::security_scheme;

  static constexpr std::string_view name = Header.view();
};

/** @brief Security scheme: a key sent as the query parameter `Param`. */
template <fixed_string Param>
struct api_key_query {
  static_assert(!Param.view().empty(), "quoll::rest::api_key_query: the name is empty");
  // It goes in a challenge's quoted string too, as it is.
  static_assert(http::detail::is_token(Param.view()), "quoll::rest::api_key_query: the name is not a valid token");

  using scheme_category = detail::security_scheme;

  static constexpr std::string_view name = Param.view();
};

/** @brief Security scheme: a bearer token in `Authorization`. */
struct bearer_auth {
  using scheme_category = detail::security_scheme;
};

/**
 * @brief An endpoint part: the security schemes the endpoint requires. They are not arguments: the server checks them
 * with validators, and a client sends the credentials it was given for each scheme.
 */
template <class... Schemes>
struct security {
  static_assert(
      (std::same_as<typename Schemes::scheme_category, detail::security_scheme> && ...),
      "quoll::rest::security: a scheme is not api_key<>, api_key_query<> or bearer_auth"
  );

  using part_category = detail::security_part;
  using schemes = detail::type_list<Schemes...>;
};

} // namespace quoll::rest

#endif // QUOLL_REST_SECURITY_HPP
