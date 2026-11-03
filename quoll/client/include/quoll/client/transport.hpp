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

#ifndef QUOLL_CLIENT_TRANSPORT_HPP
#define QUOLL_CLIENT_TRANSPORT_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/detail/connection_pool.hpp>
#include <quoll/client/detail/exchange.hpp>
#include <quoll/client/detail/host.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/client/transport_options.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/version.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/network.hpp>

#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::client {

/**
 * @brief HttpTransport speaking HTTP/1.1 to one server, over the byte streams of a connector: by default the network
 * of the executor that sends (io::network_connector), as sync_wait provides it.
 *
 * Each request goes to `host` on `port`, with a `Host` field naming them unless the request has one. Connections are
 * kept open once their response is read (keep-alive), up to `max_idle_connections`, and the next requests reuse them;
 * a request waits for no other, and opens a new connection when none is idle. A connection is not reused after a
 * request or a response that closes it (`Connection: close`, HTTP/1.0), a response delimited by the end of the stream,
 * a switch of protocol, or a stream error.
 *
 * A server may close an idle connection at any time, which shows only once a request is sent on it. Only the requests
 * whose method is idempotent (GET, HEAD, PUT, DELETE, OPTIONS, TRACE) take an idle connection: when it turns out
 * closed before any response (or answers 408 Request Timeout, sent before closing it), the request is sent again,
 * once, on a new connection, and the other idle connections, older, are closed too. The other requests (POST,
 * PATCH...), which the server may have received, open a new connection, kept afterwards for the next requests.
 *
 * Interim 1xx responses are skipped, up to 8 of them, but for a 101 switch of protocol, which is returned as the
 * response. Redirects are not followed: a 3xx response is a result like the others.
 *
 * A failure to connect is reported as the connector reports it; a failure to send the request or to read its response
 * throws std::system_error from the task, with the stream's error (`std::errc::connection_reset`...), the parser's
 * (`http::error::partial_message`, `http::error::body_too_large`, `http::error::bad_message` past 8 interim
 * responses...) or `io::error::end_of_stream`. A stop request on the awaiter's context abandons the request, with
 * `std::errc::operation_canceled`, and closes its connection. There is no timeout yet.
 *
 * Copies share their connections. The task copies what it needs: the transport does not have to outlive it.
 *
 * Thread safety: send() may be called from any thread, concurrently, if the connector allows it.
 */
template <io::Connector Connector = io::network_connector>
class transport {
public:
  /**
   * @brief A transport to `host` (a name, or an address; IPv6 without brackets) on `port`.
   *
   * @throws std::invalid_argument for a host that is neither a name (letters, digits, `-`, `_` and dots), an IPv4 nor
   * an IPv6 address: one with a port (`localhost:8080`), brackets, a zone index, spaces...
   */
  transport(Connector connector, std::string host, std::uint16_t port, transport_options options = {})
      : state_(std::make_shared<state>(std::move(connector), std::move(host), port, options)) {}

  /**
   * @brief A transport to the server of `url`, `http://host[:port]` (an IPv6 address in brackets), connecting with a
   * default-constructed connector: by default through the network of the executor that sends (io::network_connector),
   * as sync_wait provides it.
   *
   * @code
   * quoll::client::transport<> transport{"http://127.0.0.1:8080"};
   * @endcode
   *
   * @throws std::invalid_argument for another URL: `https` and a path prefix are not supported yet.
   */
  explicit transport(std::string_view url, transport_options options = {})
    requires std::default_initializable<Connector>
      : transport(Connector{}, detail::parse_url(url), options) {}

  /**
   * @brief Sends `request` and produces its response.
   *
   * @throws std::invalid_argument here, before the task runs, for a target not in origin form (starting with `/`), and
   * for what http::serialize() rejects.
   */
  [[nodiscard]] async::task<http::response> send(http::request request) const {
    if (!request.target.starts_with('/')) {
      throw std::invalid_argument{"quoll::client::transport: the target is not in origin form"};
    }

    if (!request.fields.contains("Host")) {
      request.fields.add("Host", state_->host_field);
    }

    auto const keep_alive = request.version == http::version::http_1_1 &&
                            !http::detail::closes_connection(request.fields) &&
                            state_->options.max_idle_connections > 0;

    return send_prepared(state_, detail::prepared_request{http::serialize(request), request.method, keep_alive});
  }

private:
  transport(Connector connector, detail::origin server, transport_options options)
      : transport(std::move(connector), std::move(server.host), server.port, options) {}

  struct state {
    state(Connector connector_, std::string host_, std::uint16_t port_, transport_options const &options_)
        : connector(std::move(connector_)), host(std::move(host_)), port(port_),
          host_field(detail::host_field(host, port)), options(options_), pool(options_.max_idle_connections) {}

    Connector connector;
    std::string host;
    std::uint16_t port;
    std::string host_field;
    transport_options options;
    detail::connection_pool pool;
  };

  // Static: the task holds the shared state, not the transport.
  static async::task<http::response> send_prepared(std::shared_ptr<state> shared, detail::prepared_request request) {
    if (detail::idempotent(request.method)) {
      if (auto idle = shared->pool.take()) {
        auto response =
            co_await detail::exchange(shared->pool, shared->options.limits, std::move(*idle), request, true);

        if (response) {
          co_return std::move(*response);
        }

        // Closed by the server after some idle time: the connections idle for longer are likely closed as well.
        shared->pool.clear();
      }
    }

    auto connection = co_await shared->connector.connect(shared->host, shared->port);
    auto response =
        co_await detail::exchange(shared->pool, shared->options.limits, std::move(connection), request, false);

    // Without retry_if_closed, exchange() produces a response or throws.
    assert(response && "client::detail::exchange() without retry_if_closed produced no response");
    co_return std::move(*response);
  }

  std::shared_ptr<state> state_;
};

} // namespace quoll::client

#endif // QUOLL_CLIENT_TRANSPORT_HPP
