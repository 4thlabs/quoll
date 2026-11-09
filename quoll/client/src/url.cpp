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

#include <quoll/client/detail/url.hpp>

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/plain_segment.hpp>
#include <quoll/http/detail/route_template.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quoll::http::detail {

namespace {

// The position of `character` in `text` from `from`, or the size of `text`.
[[nodiscard]] constexpr std::size_t end_of(std::string_view text, char character, std::size_t from) noexcept {
  while (from < text.size() && text[from] != character) {
    ++from;
  }

  return from;
}

// A host name or an IPv4 address: letters, digits, '-', '_' and the dots between labels.
[[nodiscard]] constexpr bool valid_host_name(std::string_view host) noexcept {
  if (host.empty()) {
    return false;
  }

  auto label_begun = false;

  for (auto const character : host) {
    if (character == '.') {
      // Between labels, with a last one that may end a name.
      if (!label_begun) {
        return false;
      }

      label_begun = false;
      continue;
    }

    auto const allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') || character == '-' || character == '_';

    if (!allowed) {
      return false;
    }

    label_begun = true;
  }

  return true;
}

// Four decimal numbers up to 255, separated by dots.
[[nodiscard]] constexpr bool valid_ipv4(std::string_view text) noexcept {
  std::size_t index = 0;

  for (int part = 0; part < 4; ++part) {
    auto const end = end_of(text, '.', index);

    if (end == index || end - index > 3 || (part < 3) != (end < text.size())) {
      return false;
    }

    int value = 0;

    for (auto const character : text.substr(index, end - index)) {
      if (character < '0' || character > '9') {
        return false;
      }

      value = value * 10 + (character - '0');
    }

    if (value > 255) {
      return false;
    }

    index = end + 1;
  }

  return true;
}

// The text form of an IPv6 address (RFC 4291): eight groups of 1 to 4 hex digits separated by colons, one "::"
// standing for one or more groups of zeros, the last two groups possibly written as an IPv4 address. No zone index.
[[nodiscard]] constexpr bool valid_ipv6(std::string_view text) noexcept {
  std::size_t groups = 0;
  std::size_t index = 0;
  auto compressed = false;

  if (text.starts_with("::")) {
    compressed = true;
    index = 2;
  } else if (text.starts_with(':')) {
    return false;
  }

  while (index < text.size()) {
    auto const end = end_of(text, ':', index);
    auto const group = text.substr(index, end - index);

    if (end == text.size() && group.contains('.')) {
      if (!valid_ipv4(group)) {
        return false;
      }

      groups += 2;
      break;
    }

    if (group.empty() || group.size() > 4) {
      return false;
    }

    for (auto const character : group) {
      if (!http::detail::is_hex_digit(character)) {
        return false;
      }
    }

    ++groups;

    if (end == text.size()) {
      break;
    }

    index = end + 1;

    if (index == text.size()) {
      // A single trailing colon.
      return false;
    }

    if (text[index] == ':') {
      if (compressed) {
        return false;
      }

      compressed = true;
      ++index;
    }
  }

  return compressed ? groups < 8 : groups == 8;
}

[[noreturn]] void invalid_url(std::string_view why) {
  throw std::invalid_argument{"quoll::http::client: " + std::string{why}};
}

// A decimal port, 1 to 65535.
[[nodiscard]] constexpr std::optional<std::uint16_t> parse_port(std::string_view text) noexcept {
  if (text.empty() || text.size() > 5) {
    return std::nullopt;
  }

  unsigned value = 0;

  for (auto const character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }

    value = value * 10 + static_cast<unsigned>(character - '0');
  }

  if (value == 0 || value > 65535) {
    return std::nullopt;
  }

  return static_cast<std::uint16_t>(value);
}

struct authority_parts {
  std::string_view host;
  std::optional<std::uint16_t> port;
};

// `host[:port]` or `[ipv6][:port]`, brackets holding an IPv6 address only (RFC 3986): nothing when it is neither.
[[nodiscard]] constexpr std::optional<authority_parts>
split_authority(std::string_view authority QUOLL_LIFETIMEBOUND) noexcept {
  auto host = authority;
  std::string_view rest;

  if (authority.starts_with('[')) {
    auto const closing = authority.find(']');

    if (closing == std::string_view::npos) {
      return std::nullopt;
    }

    host = authority.substr(1, closing - 1);
    rest = authority.substr(closing + 1);

    if (!host.contains(':') || (!rest.empty() && !rest.starts_with(':'))) {
      return std::nullopt;
    }
  } else if (auto const colon = authority.find(':'); colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    rest = authority.substr(colon);
  }

  if (rest.empty()) {
    return authority_parts{host, std::nullopt};
  }

  auto const port = parse_port(rest.substr(1));

  if (!port) {
    return std::nullopt;
  }

  return authority_parts{host, port};
}

// A character RFC 3986 allows in a path as it is: unreserved, sub-delims, `:` and `@`, and the separators.
[[nodiscard]] constexpr bool path_character(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || std::string_view{"-._~!$&'()*+,;=:@/%"}.contains(character);
}

// A character RFC 3986 allows in a reference without its fragment, or `[` and `]`, which a query often holds as they
// are, and which a path may not. A backslash is not one, which some clients read as a slash, and so is no way to reach
// another server.
[[nodiscard]] constexpr bool reference_character(char character) noexcept {
  return path_character(character) || character == '?' || character == '[' || character == ']';
}

// Whether `authority` names what the `Host` field `host` does. Credentials end up in the host, or in the port, and
// differ.
[[nodiscard]] constexpr bool same_authority(std::string_view authority, std::string_view host,
                                            std::uint16_t default_port) noexcept {
  auto const named = split_authority(authority);
  auto const asked = split_authority(host);

  return named && asked && http::detail::iequals(named->host, asked->host) &&
         named->port.value_or(default_port) == asked->port.value_or(default_port);
}

// `path`, starting with a slash, without its `.` and `..` segments (RFC 3986, 5.2.4).
[[nodiscard]] std::string remove_dot_segments(std::string_view path) {
  std::string result;

  for (std::size_t start = 0; start < path.size();) {
    auto const end = end_of(path, '/', start + 1);
    auto const segment = path.substr(start + 1, end - start - 1);
    auto const last = end == path.size();

    if (segment == "." || segment == "..") {
      if (segment == ".." && !result.empty()) {
        result.erase(result.rfind('/'));
      }

      if (last) {
        result += '/';
      }
    } else {
      result += '/';
      result += segment;
    }

    start = end;
  }

  return result;
}

// What a proxy or a framework would read as another server, or cut short, once it decodes and normalizes the path of a
// target: a URL parser drops a tab or a line break, a backslash reads as a slash, and dot segments go. A NUL ends it
// there.
[[nodiscard]] std::optional<std::string> read_as_a_parser_does(std::string_view path) {
  auto decoded = http::detail::percent_decode(path);

  if (!decoded || decoded->contains('\0')) {
    return std::nullopt;
  }

  std::erase_if(*decoded,
                [](char const character) { return character == '\t' || character == '\n' || character == '\r'; });
  std::ranges::replace(*decoded, '\\', '/');

  return decoded;
}

[[nodiscard]] bool reads_as_itself(std::string_view path) {
  auto const read = read_as_a_parser_does(path);

  return read && !read->starts_with("//") && !remove_dot_segments(*read).starts_with("//");
}

// A path to put before every target: it goes on the wire as it is written, so that it has to be what a server reads.
void check_prefix(std::string_view path) {
  if (!std::ranges::all_of(path, path_character) || !http::detail::valid_escapes(path) || path.contains("//")) {
    invalid_url("a path that would not read as itself");
  }

  // The escapes are valid: checked above.
  auto const read = read_as_a_parser_does(path.ends_with('/') ? path.substr(0, path.size() - 1) : path);

  if (!read) {
    invalid_url("a NUL in the path");
  }

  // With what comes after it: `/%09` alone reads as nothing, and before `/x` as another server.
  auto const followed = *read + '/';

  if (followed.contains("//") || remove_dot_segments(followed).starts_with("//")) {
    invalid_url("a path a proxy could read as another server");
  }

  // A segment of it is judged as a router does, an encoded slash included.
  if (!http::detail::plain_segment(followed)) {
    invalid_url("a dot segment in the path");
  }
}

} // namespace

void check_origin_form(std::string_view target) {
  if (!target.starts_with('/')) {
    throw std::invalid_argument{"quoll::http::client: the target is not in origin form"};
  }
}

websocket_target parse_websocket_url(std::string_view url) {
  constexpr std::string_view plain_scheme = "ws://";
  constexpr std::string_view tls_scheme = "wss://";

  auto const secure =
      url.size() >= tls_scheme.size() && http::detail::iequals(url.substr(0, tls_scheme.size()), tls_scheme);

  if (!secure &&
      (url.size() < plain_scheme.size() || !http::detail::iequals(url.substr(0, plain_scheme.size()), plain_scheme))) {
    throw std::invalid_argument{"quoll::http::connect_websocket: the URL is not ws[s]://host[:port][/path][?query]"};
  }

  auto const rest = url.substr(secure ? tls_scheme.size() : plain_scheme.size());

  if (rest.find('#') != std::string_view::npos) {
    throw std::invalid_argument{"quoll::http::connect_websocket: a WebSocket URL has no fragment"};
  }

  // A query is checked as a request's target is, as the handshake is built.
  auto const question = rest.find('?');
  auto const query = question == std::string_view::npos ? std::string_view{} : rest.substr(question);
  auto const before = rest.substr(0, question);
  auto const slash = before.find('/');
  auto const path = slash == std::string_view::npos ? std::string_view{"/"} : before.substr(slash);

  // Its server checked as an http one's is, what is refused said for connect_websocket. Its path is no prefix with a
  // boundary to keep, but the whole target, the caller's own: checked by the serializer, as any request's.
  origin server;

  try {
    // The authority alone: the path is a target, which the handshake's serializer checks as one, not a prefix.
    server =
        parse_url((secure ? std::string{"https://"} : std::string{"http://"}) + std::string{before.substr(0, slash)});
    static_cast<void>(host_field(server.host, server.port));
  } catch (std::invalid_argument const &refused) {
    constexpr std::string_view client_prefix = "quoll::http::client: ";
    std::string_view reason = refused.what();

    if (reason.starts_with(client_prefix)) {
      reason.remove_prefix(client_prefix.size());
    }

    throw std::invalid_argument{"quoll::http::connect_websocket: " + std::string{reason}};
  }

  websocket_target result{std::move(server), std::string{path} + std::string{query}};

  return result;
}

origin parse_url(std::string_view url) {
  constexpr std::string_view plain_scheme = "http://";
  constexpr std::string_view tls_scheme = "https://";

  auto const secure =
      url.size() >= tls_scheme.size() && http::detail::iequals(url.substr(0, tls_scheme.size()), tls_scheme);

  if (!secure &&
      (url.size() < plain_scheme.size() || !http::detail::iequals(url.substr(0, plain_scheme.size()), plain_scheme))) {
    invalid_url("the URL is not http[s]://host[:port][/path]");
  }

  auto authority = url.substr(secure ? tls_scheme.size() : plain_scheme.size());

  if (authority.find_first_of("?#") != std::string_view::npos) {
    invalid_url("a query or a fragment is not part of a server's URL");
  }

  auto const slash = authority.find('/');
  auto const path = slash == std::string_view::npos ? std::string_view{} : authority.substr(slash);

  check_prefix(path);
  authority = authority.substr(0, slash);

  if (authority.find('@') != std::string_view::npos) {
    invalid_url("credentials in the URL are not supported; send them with the requests");
  }

  origin result;

  // `/v1/` and `/v1` are one prefix, and `/` is none: a target starts with its own slash.
  result.prefix = path.ends_with('/') ? path.substr(0, path.size() - 1) : path;

  result.port = secure ? 443 : 80;
  result.secure = secure;

  auto const parts = split_authority(authority);

  if (!parts) {
    invalid_url("invalid host or port in " + std::string{authority});
  }

  result.host = parts->host;

  if (parts->port) {
    result.port = *parts->port;
  }

  return result;
}

std::string host_field(std::string_view host, std::uint16_t port, std::uint16_t default_port) {
  auto const ipv6 = host.contains(':');

  if (ipv6 ? !valid_ipv6(host) : !valid_host_name(host)) {
    throw std::invalid_argument{"quoll::http::client: invalid host"};
  }

  auto field = ipv6 ? "[" + std::string{host} + "]" : std::string{host};

  if (port != default_port) {
    field += ':' + std::to_string(port);
  }

  return field;
}

std::optional<std::string> resolve_location(std::string_view location, std::string_view host, bool secure,
                                            std::string_view target) {
  auto reference = location.substr(0, location.find('#'));

  if (!std::ranges::all_of(reference, reference_character) || !http::detail::valid_escapes(reference)) {
    return std::nullopt;
  }

  // RFC 3986 reads an empty reference as the resource itself: a server saying so has not said where to go, and
  // following it would send the same request again, a POST too, up to the limit.
  if (reference.empty()) {
    return std::nullopt;
  }

  auto const scheme_end = reference.find_first_of(":/?");

  if (scheme_end != std::string_view::npos && reference[scheme_end] == ':') {
    if (!http::detail::iequals(reference.substr(0, scheme_end), secure ? "https" : "http")) {
      return std::nullopt;
    }

    reference.remove_prefix(scheme_end + 1);

    if (!reference.starts_with("//")) {
      return std::nullopt;
    }
  }

  auto named_server = false;

  if (reference.starts_with("//")) {
    reference.remove_prefix(2);

    auto const authority = reference.substr(0, reference.find_first_of("/?"));

    if (!same_authority(authority, host, secure ? 443 : 80)) {
      return std::nullopt;
    }

    reference.remove_prefix(authority.size());
    named_server = true;
  }

  auto const query_start = reference.find('?');
  auto const reference_path = reference.substr(0, query_start);
  auto const query = query_start == std::string_view::npos ? std::string_view{} : reference.substr(query_start);
  auto const base_path = target.substr(0, target.find_first_of("?#"));

  if (reference_path.find_first_of("[]") != std::string_view::npos) {
    return std::nullopt;
  }

  std::string resolved;

  if (reference_path.empty()) {
    // A query alone keeps the path as it was sent (RFC 3986, 5.2.2).
    resolved = named_server ? std::string_view{"/"} : base_path;
  } else if (reference_path.starts_with('/')) {
    resolved = remove_dot_segments(reference_path);
  } else {
    auto merged = std::string{base_path.substr(0, base_path.rfind('/') + 1)};

    merged += reference_path;
    resolved = remove_dot_segments(merged);
  }

  if (!reads_as_itself(resolved)) {
    return std::nullopt;
  }

  resolved += query;
  return resolved;
}

} // namespace quoll::http::detail
