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

#include <quoll/client/connect_websocket.hpp>

#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/websocket_key.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/websocket_handshake.hpp>
#include <quoll/http/websocket_parser.hpp>
#include <quoll/io/network_connector.hpp>
#include <quoll/io/write.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::http {

namespace {

constexpr std::array handshake_fields{std::string_view{"Host"},
                                      std::string_view{"Upgrade"},
                                      std::string_view{"Connection"},
                                      std::string_view{"Sec-WebSocket-Key"},
                                      std::string_view{"Sec-WebSocket-Version"},
                                      std::string_view{"Sec-WebSocket-Extensions"}};

// Whether `token` is among the comma-separated subprotocols `fields` offer.
[[nodiscard]] bool offered(http::fields const &fields, std::string_view token) {
  for (auto const &field : fields) {
    if (!detail::iequals(field.name, "Sec-WebSocket-Protocol")) {
      continue;
    }

    for (auto const member : std::views::split(std::string_view{field.value}, ',')) {
      if (detail::trim(std::string_view{member.begin(), member.end()}) == token) {
        return true;
      }
    }
  }

  return false;
}

// Whether `answer` switches to the WebSocket the handshake with `key` and `fields` asked for (RFC 6455, 4.1).
[[nodiscard]] bool switches(http::response const &answer, std::string_view key, http::fields const &fields) {
  if (answer.status != status::switching_protocols || !detail::field_names(answer.fields, "Upgrade", "websocket") ||
      !detail::connection_names(answer.fields, "upgrade")) {
    return false;
  }

  auto const accept = answer.fields.find("Sec-WebSocket-Accept");

  if (!accept || detail::trim(*accept) != websocket_accept(key)) {
    return false;
  }

  // None was offered.
  if (answer.fields.contains("Sec-WebSocket-Extensions")) {
    return false;
  }

  auto const protocol = answer.fields.find("Sec-WebSocket-Protocol");

  return !protocol || offered(fields, detail::trim(*protocol));
}

// Interim answers skipped before the 101, against a server sending them for ever.
constexpr int max_interim_answers = 8;

async::task<connected_websocket> shake_hands(detail::websocket_opener open, detail::websocket_handshake handshake,
                                             websocket_connect_options options) {
  auto stream = co_await open();
  // Kept rather than thrown at once: a server refusing as it reads may have answered, which says more.
  auto const unsent = (co_await io::write(stream, std::as_bytes(std::span{handshake.wire}))).error;
  std::string buffer;

  for (auto interim = 0;; ++interim) {
    response_parser parser{options.limits};

    if (auto const failed = co_await http::read(stream, buffer, parser)) {
      throw std::system_error{unsent ? unsent : failed,
                              unsent ? "quoll::http::connect_websocket: cannot send the handshake"
                                     : "quoll::http::connect_websocket: cannot read the server's answer"};
    }

    auto answer = parser.release();

    if (is_informational(answer.status) && answer.status != status::switching_protocols) {
      if (interim == max_interim_answers) {
        throw std::system_error{make_error_code(error::bad_message),
                                "quoll::http::connect_websocket: too many interim answers"};
      }

      continue;
    }

    if (unsent || !switches(answer, handshake.key, handshake.offered)) {
      throw websocket_refused{std::move(answer)};
    }

    // What came after the 101 is the WebSocket's.
    co_return connected_websocket{
        websocket{std::move(stream), websocket_role::client, std::move(buffer), options.websocket}, std::move(answer)};
  }
}

} // namespace

websocket_refused::websocket_refused(http::response answer)
    : std::runtime_error{"quoll::http::connect_websocket: the server answered " +
                         std::to_string(static_cast<int>(answer.status)) + ", not the WebSocket asked for"},
      response_(std::move(answer)) {}

namespace detail {

websocket_handshake prepare_websocket_handshake(websocket_target const &where, http::fields fields) {
  for (auto const &field : fields) {
    if (std::ranges::any_of(handshake_fields, [&field](std::string_view name) { return iequals(field.name, name); })) {
      throw std::invalid_argument{"quoll::http::connect_websocket: a field of the handshake's own: " + field.name};
    }
  }

  websocket_handshake handshake{.wire = {}, .key = random_websocket_key(), .offered = fields};
  request message;

  message.method = verb::get;
  message.target = where.target;
  message.fields = std::move(fields);
  message.fields.set("Host", host_field(where.server.host, where.server.port, where.server.secure ? 443 : 80));
  message.fields.set("Upgrade", "websocket");
  message.fields.set("Connection", "Upgrade");
  message.fields.set("Sec-WebSocket-Key", handshake.key);
  message.fields.set("Sec-WebSocket-Version", "13");
  handshake.wire = serialize(message);
  return handshake;
}

async::task<connected_websocket> connect_websocket_through(websocket_opener open, websocket_handshake handshake,
                                                           websocket_connect_options options) {
  auto const limit = options.handshake_timeout;

  co_return co_await async::detail::with_optional_timeout(
      limit, shake_hands(std::move(open), std::move(handshake), std::move(options)));
}

} // namespace detail

async::task<connected_websocket> connect_websocket(std::string_view url, websocket_connect_options options) {
  return connect_websocket(io::network_connector{}, url, std::move(options));
}

} // namespace quoll::http
