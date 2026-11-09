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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::http::detail {

// Throws std::invalid_argument for a target that does not start with a slash.
void check_origin_form(std::string_view target);

// The server an `http[s]://host[:port][/path]` URL names. The host is checked by host_field().
struct origin {
  std::string host;
  std::uint16_t port = 80;
  bool secure = false;
  // What comes before the target of every request: `/v1`, with no slash at its end; empty without one.
  std::string prefix{};
};

// `http[s]://host[:port]`, optionally followed by a path, an IPv6 address in brackets (`http://[::1]:8080`). A path
// that would not read as itself is refused: a character RFC 3986 does not allow there, an invalid escape, a dot
// segment, encoded or not, a NUL, or an empty segment.
[[nodiscard]] origin parse_url(std::string_view url);

// Where a WebSocket URL leads: its server, and the target of its handshake, path and query.
struct websocket_target {
  origin server;
  std::string target;
};

// `ws[s]://host[:port][/path][?query]`, as RFC 6455, 3 has it: no fragment.
[[nodiscard]] websocket_target parse_websocket_url(std::string_view url);

// The Host field naming `host` on `port`: an IPv6 address in brackets, the port unless it is the scheme's default.
// Throws std::invalid_argument for a host that is neither a name, an IPv4 nor an IPv6 address.
[[nodiscard]] std::string host_field(std::string_view host, std::uint16_t port, std::uint16_t default_port = 80);

// The target a `Location` field asks for (RFC 3986, 5.2), from a request sent to `target` with the `Host` field `host`,
// in origin form: no fragment, and dot segments resolved, unless it is a query alone, which keeps the path as it was
// sent. Nothing when it names another scheme than `secure` says, another host or port than `host`, says nothing but a
// fragment, holds what RFC 3986 does not allow in a reference, or leads to a path that, once decoded, starts with two
// slashes or a slash and a backslash, whatever a URL parser drops and dot segments apart, or holds a NUL.
[[nodiscard]] std::optional<std::string> resolve_location(std::string_view location, std::string_view host, bool secure,
                                                          std::string_view target);

} // namespace quoll::http::detail

#endif // QUOLL_CLIENT_DETAIL_URL_HPP
