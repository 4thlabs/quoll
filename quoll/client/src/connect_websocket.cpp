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

#include <quoll/client/detail/connect_websocket.hpp>
#include <quoll/client/websocket_refused.hpp>

#include <quoll/client/detail/max_interim_responses.hpp>
#include <quoll/client/detail/peer_has_gone.hpp>

#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/websocket_key.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/http/websocket_handshake.hpp>
#include <quoll/io/half_closable_stream.hpp>
#include <quoll/io/network_connector.hpp>
#include <quoll/io/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace quoll::http {

namespace {

// The handshake's own fields, and what would give it a body, which it has none of (RFC 6455, 4.1).
constexpr std::array handshake_fields{std::string_view{"Host"},
                                      std::string_view{"Upgrade"},
                                      std::string_view{"Connection"},
                                      std::string_view{"Sec-WebSocket-Key"},
                                      std::string_view{"Sec-WebSocket-Version"},
                                      std::string_view{"Sec-WebSocket-Extensions"},
                                      std::string_view{"Sec-WebSocket-Accept"},
                                      std::string_view{"Content-Length"},
                                      std::string_view{"Transfer-Encoding"}};

// Whether `answer` switches to the WebSocket the handshake with `key`, offering `offered`, asked for (RFC 6455, 4.1).
// A subprotocol is compared as it is, as RFC 6455 has them.
[[nodiscard]] bool switches(http::response const &answer, std::string_view key,
                            std::vector<std::string> const &offered) {
  // One of each, as two would leave which one counts to whoever reads them.
  auto const count = [&answer](std::string_view name) {
    return std::ranges::count_if(answer.fields,
                                 [name](auto const &field) { return detail::iequals(field.name, name); });
  };

  // `websocket` alone, whatever its case (RFC 6455, 4.1), in one Upgrade field.
  auto const upgrade = answer.fields.find("Upgrade");

  // HTTP/1.1, as HTTP/1.0 has no switch (RFC 9110, 7.8).
  if (answer.status != status::switching_protocols || answer.version != version::http_1_1 || count("Upgrade") != 1 ||
      !detail::iequals(detail::trim(*upgrade), "websocket") || !detail::connection_names(answer.fields, "upgrade")) {
    return false;
  }

  auto const accept = answer.fields.find("Sec-WebSocket-Accept");

  if (count("Sec-WebSocket-Accept") != 1 || count("Sec-WebSocket-Protocol") > 1 ||
      detail::trim(*accept) != websocket_accept(key)) {
    return false;
  }

  // None was offered.
  if (answer.fields.contains("Sec-WebSocket-Extensions")) {
    return false;
  }

  auto const protocol = answer.fields.find("Sec-WebSocket-Protocol");

  return !protocol || std::ranges::find(offered, detail::trim(*protocol)) != offered.end();
}

// A stream that keeps the connector that opened it alive, as an http::client keeps its own for as long as its
// connections: a stream may point into what its connector owns. The stream goes first. Thread safety: the stream's.
class keeping_connector {
public:
  keeping_connector(io::any_stream stream, std::shared_ptr<void const> connector) noexcept
      : connector_(std::move(connector)), stream_(std::move(stream)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    return stream_.read_some(buffer);
  }

  [[nodiscard]] auto write_some(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    return stream_.write_some(data);
  }

  [[nodiscard]] auto shutdown_send() QUOLL_LIFETIMEBOUND { return stream_.shutdown_send(); }

private:
  std::shared_ptr<void const> connector_;
  io::any_stream stream_;
};

static_assert(io::HalfClosableStream<keeping_connector>);

// What broke the handshake as it was sent.
[[noreturn]] void throw_unsent(std::error_code unsent) {
  throw std::system_error{unsent, "quoll::http::connect_websocket: cannot send the handshake"};
}

[[nodiscard]] async::task<connected_websocket> shake_hands(detail::connector_handle connector, detail::origin server,
                                                           detail::websocket_handshake handshake,
                                                           websocket_connect_options options) {
  auto stream =
      co_await connector.open(connector.connector.get(), server.host, server.port, server.secure, options.tls);
  // Kept rather than thrown at once when the peer has gone: a server refusing as it read may have answered, which says
  // more. Any other failure is this side's, and nothing would come.
  auto const unsent = (co_await io::write(stream, std::as_bytes(std::span{handshake.wire}))).error;

  if (unsent && !detail::peer_has_gone(unsent)) {
    throw_unsent(unsent);
  }

  std::string buffer;

  for (auto interim = 0;; ++interim) {
    response_parser parser{options.limits};

    // A final answer's body is only a refusal's, judged by its head: not read, as one may end only with the connection,
    // or be large. An interim one is read whole, as http::client does, so that what follows it is the next answer.
    parser.set_body_streaming([](response const &head) { return !is_informational(head.status); });

    if (auto const failed = co_await http::read(stream, buffer, parser)) {
      // The write's failure, when what ended the read is only the peer having gone.
      auto const sending = unsent && detail::peer_has_gone(failed);

      if (sending) {
        throw_unsent(unsent);
      }

      throw std::system_error{failed, "quoll::http::connect_websocket: cannot read the server's answer"};
    }

    // Its head alone, what came of a refusal's body with it dropped, as how much did depends on how the bytes came.
    auto answer = parser.done() ? parser.release() : parser.head();

    answer.body.clear();

    if (is_informational(answer.status) && answer.status != status::switching_protocols) {
      if (interim == detail::max_interim_responses) {
        throw std::system_error{make_error_code(error::bad_message),
                                "quoll::http::connect_websocket: too many interim answers"};
      }

      continue;
    }

    if (!switches(answer, handshake.key, handshake.offered)) {
      throw websocket_refused{std::move(answer)};
    }

    // A switch to a handshake that could not be sent whole is no connection to use: what broke it says why.
    if (unsent) {
      throw_unsent(unsent);
    }

    // What came after the 101 is the WebSocket's.
    co_return connected_websocket{
        websocket{io::any_stream{keeping_connector{std::move(stream), std::move(connector.connector)}},
                  websocket_role::client, std::move(buffer), options.websocket},
        std::move(answer)};
  }
}

} // namespace

namespace detail {

websocket_handshake prepare_websocket_handshake(websocket_target const &where, http::fields fields) {
  websocket_handshake handshake{.wire = {}, .key = random_websocket_key(), .offered = {}};

  for (auto const &field : fields) {
    if (std::ranges::any_of(handshake_fields, [&field](std::string_view name) { return iequals(field.name, name); })) {
      throw std::invalid_argument{"quoll::http::connect_websocket: a field of the handshake's own: " + field.name};
    }

    if (!iequals(field.name, "Sec-WebSocket-Protocol")) {
      continue;
    }

    auto const before = handshake.offered.size();

    for (auto const member : std::views::split(std::string_view{field.value}, ',')) {
      // An empty member offers nothing, nor may a server choose it.
      auto const token = trim(std::string_view{member.begin(), member.end()});

      if (token.empty()) {
        continue;
      }

      // One no server could choose (RFC 6455, 4.1: a token).
      if (!is_token(token)) {
        throw std::invalid_argument{"quoll::http::connect_websocket: a subprotocol that is no token: " +
                                    std::string{token}};
      }

      // Each one once (RFC 6455, 4.1).
      if (std::ranges::find(handshake.offered, token) != handshake.offered.end()) {
        throw std::invalid_argument{"quoll::http::connect_websocket: a subprotocol offered twice: " +
                                    std::string{token}};
      }

      handshake.offered.emplace_back(token);
    }

    if (handshake.offered.size() == before) {
      throw std::invalid_argument{"quoll::http::connect_websocket: a Sec-WebSocket-Protocol that names none"};
    }
  }

  request message;

  message.method = verb::get;
  message.target = where.target;
  message.fields = std::move(fields);

  // As checked: the tokens once each, with no empty member, which a sender may not write (RFC 9110, 5.6.1).
  if (!handshake.offered.empty()) {
    std::string offer;

    for (auto const &token : handshake.offered) {
      if (!offer.empty()) {
        offer += ", ";
      }

      offer += token;
    }

    // One field in place of all the offer's.
    message.fields.set("Sec-WebSocket-Protocol", std::move(offer));
  }

  message.fields.set("Host", host_field(where.server.host, where.server.port, where.server.secure ? 443 : 80));
  message.fields.set("Upgrade", "websocket");
  message.fields.set("Connection", "Upgrade");
  message.fields.set("Sec-WebSocket-Key", handshake.key);
  message.fields.set("Sec-WebSocket-Version", "13");

  // What the serializer refuses, said for connect_websocket.
  try {
    handshake.wire = serialize(message);
  } catch (std::invalid_argument const &refused) {
    constexpr std::string_view http_prefix = "quoll::http: ";
    std::string_view reason = refused.what();

    if (reason.starts_with(http_prefix)) {
      reason.remove_prefix(http_prefix.size());
    }

    throw std::invalid_argument{"quoll::http::connect_websocket: " + std::string{reason}};
  }

  return handshake;
}

async::task<connected_websocket> connect_websocket_through(connector_handle connector, origin server,
                                                           websocket_handshake handshake,
                                                           websocket_connect_options options) {
  // Read before what holds it is moved; every argument goes by value into the task.
  // Zero or less: no limit.
  auto const limit = std::max(options.handshake_timeout, std::chrono::nanoseconds::zero());

  return async::detail::with_optional_timeout(
      limit, shake_hands(std::move(connector), std::move(server), std::move(handshake), std::move(options)));
}

} // namespace detail

async::task<connected_websocket> connect_websocket(std::string_view url, websocket_connect_options options) {
  return connect_websocket(io::network_connector{}, url, std::move(options));
}

} // namespace quoll::http
