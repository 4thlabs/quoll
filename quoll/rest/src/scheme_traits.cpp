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

#include <quoll/rest/detail/scheme_traits.hpp>

#include <quoll/http/detail/ascii.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::rest::detail {

std::optional<std::string> scheme_traits<bearer_auth>::find(http::fields const &fields,
                                                            std::vector<query_entry> const &) {
  constexpr std::string_view scheme = "Bearer ";
  auto const value = single_field(fields, "Authorization");

  if (!value || value->size() <= scheme.size() || !http::detail::iequals(value->substr(0, scheme.size()), scheme)) {
    return std::nullopt;
  }

  auto const token = http::detail::trim(value->substr(scheme.size()));

  return token.empty() ? std::nullopt : std::optional<std::string>{token};
}

std::string scheme_traits<bearer_auth>::challenge() { return "Bearer"; }

void scheme_traits<bearer_auth>::add(std::string_view value, http::fields &fields, std::string &) {
  check_credential_text(value);
  fields.add("Authorization", "Bearer " + std::string{value});
}

void check_credential_text(std::string_view text) {
  if (text.empty() || http::detail::trim(text) != text || http::detail::has_control(text)) {
    throw std::invalid_argument{"quoll::rest::client: a credential the server would read differently"};
  }
}

void check_credential_in_query(std::string_view text) {
  if (text.empty()) {
    throw std::invalid_argument{"quoll::rest::client: a credential the server would read as none"};
  }
}

} // namespace quoll::rest::detail
