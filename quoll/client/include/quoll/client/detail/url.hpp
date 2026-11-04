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

#ifndef QUOLL_CLIENT_DETAIL_URL_HPP
#define QUOLL_CLIENT_DETAIL_URL_HPP

#include <quoll/http/detail/ascii.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::client::detail {

// The server an `http[s]://host[:port]` URL names. The host is checked by host_field().
struct origin {
  std::string host;
  std::uint16_t port = 80;
  bool secure = false;
};

[[noreturn]] inline void invalid_url(std::string_view why) {
  throw std::invalid_argument{"quoll::client::transport: " + std::string{why}};
}

// A decimal port, 1 to 65535.
[[nodiscard]] inline std::uint16_t parse_port(std::string_view text) {
  if (text.empty() || text.size() > 5) {
    invalid_url("invalid port");
  }

  unsigned value = 0;

  for (auto const character : text) {
    if (character < '0' || character > '9') {
      invalid_url("invalid port");
    }

    value = value * 10 + static_cast<unsigned>(character - '0');
  }

  if (value == 0 || value > 65535) {
    invalid_url("invalid port");
  }

  return static_cast<std::uint16_t>(value);
}

// `http[s]://host[:port]`, optionally followed by `/`; an IPv6 address in brackets (`http://[::1]:8080`). No path
// prefix yet: a path is refused rather than ignored.
[[nodiscard]] inline origin parse_url(std::string_view url) {
  constexpr std::string_view http = "http://";
  constexpr std::string_view https = "https://";

  auto const secure = url.size() >= https.size() && http::detail::iequals(url.substr(0, https.size()), https);

  if (!secure && (url.size() < http.size() || !http::detail::iequals(url.substr(0, http.size()), http))) {
    invalid_url("the URL is not http[s]://host[:port]");
  }

  auto authority = url.substr(secure ? https.size() : http.size());

  if (authority.find_first_of("?#") != std::string_view::npos) {
    invalid_url("a query or a fragment is not part of a server's URL");
  }

  if (authority.find('@') != std::string_view::npos) {
    invalid_url("credentials in the URL are not supported; send them with the requests");
  }

  auto const slash = authority.find('/');

  if (slash != std::string_view::npos) {
    if (slash + 1 != authority.size()) {
      invalid_url("a path prefix is not supported yet");
    }

    authority = authority.substr(0, slash);
  }

  origin result;

  result.port = secure ? 443 : 80;
  result.secure = secure;

  if (authority.starts_with('[')) {
    auto const closing = authority.find(']');

    if (closing == std::string_view::npos) {
      invalid_url("unclosed bracket");
    }

    result.host = authority.substr(1, closing - 1);

    // Brackets hold an IPv6 address only (RFC 3986): a name or an IPv4 address goes without.
    if (!result.host.contains(':')) {
      invalid_url("brackets around a host that is not an IPv6 address");
    }

    auto const rest = authority.substr(closing + 1);

    if (!rest.empty()) {
      if (!rest.starts_with(':')) {
        invalid_url("invalid authority");
      }

      result.port = parse_port(rest.substr(1));
    }

    return result;
  }

  auto const colon = authority.find(':');

  result.host = authority.substr(0, colon);

  if (colon != std::string_view::npos) {
    result.port = parse_port(authority.substr(colon + 1));
  }

  return result;
}

} // namespace quoll::client::detail

#endif // QUOLL_CLIENT_DETAIL_URL_HPP
