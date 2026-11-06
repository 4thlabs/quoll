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
#include <quoll/client/detail/url.hpp>
#include <quoll/client/transport_options.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/network_connector.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::client {

namespace detail {

// What a transport does, whatever its connector: compiled once.
class transport_core;

// A connector, whichever it is: the core owns it through this.
struct connector_handle {
  std::shared_ptr<void const> connector;
  async::task<io::any_stream> (*open)(void const *connector, std::string const &host, std::uint16_t port, bool secure,
                                      io::tls_client_options const &tls);
};

[[nodiscard]] std::shared_ptr<transport_core> make_transport_core(connector_handle connector, std::string host,
                                                                  std::uint16_t port, bool secure, std::string prefix,
                                                                  transport_options const &options);

[[nodiscard]] async::task<http::response> send(std::shared_ptr<transport_core> const &core, http::request request);
[[nodiscard]] async::task<http::streamed_response> stream(std::shared_ptr<transport_core> const &core,
                                                          http::request request);

} // namespace detail

/**
 * @brief HttpTransport speaking HTTP/1.1 to one server, over the byte streams of a connector: by default the network
 * of the executor that sends (io::network_connector), as quoll::runtime provides it.
 *
 * Each request goes to `host` on `port`, with a `Host` field naming them unless the request has one. Connections are
 * kept open once their response is read (keep-alive), up to `max_idle_connections`, and the next requests reuse them;
 * a request waits for no other, unless transport_options::max_connections says so, and opens a new connection when none
 * is idle. A connection is not reused after a request or a response that closes it (`Connection: close`, HTTP/1.0), a
 * response delimited by the end of the stream, a switch of protocol, or a stream error.
 *
 * A server may close an idle connection at any time, which shows only once a request is sent on it. Only the requests
 * whose method is idempotent (GET, HEAD, PUT, DELETE, OPTIONS, TRACE) take an idle connection: when it turns out
 * closed before any response (or answers 408 Request Timeout, sent before closing it), the request is sent again,
 * once, on a new connection, and the other idle connections, older, are closed too. The other requests (POST,
 * PATCH...), which the server may have received, open a new connection, kept afterwards for the next requests.
 *
 * Over TLS, a server closing without TLS's closing message is `std::errc::connection_reset`. It changes nothing for a
 * response whose length is known, nor for a kept connection, but it fails a response delimited by the end of the
 * stream: anyone on the way could have cut it, and it would look complete.
 *
 * Interim 1xx responses are skipped, up to 8 of them, but for a 101 switch of protocol, which is returned as the
 * response.
 *
 * Redirects are not followed unless transport_options::max_redirects says so: a 3xx response is a result like the
 * others. With it, a 301, 302, 303, 307 or 308 response whose `Location` names the transport's scheme and the host and
 * port of the request's `Host` field, by default the transport's, sends the request again there, its target being what
 * `Location` says, whatever the path of the transport's URL. 307 and 308 keep the method. 303 makes it a GET, but for a
 * HEAD, and 301 and 302 make a POST a GET, as browsers do. When the method changes, the body goes, with its
 * `Content-*`, `Digest`, `ETag`, `Last-Modified`, `Transfer-Encoding`, `Trailer` and `Expect` fields, and so do the
 * preconditions (`If-*`, `Range`). The other fields stay, `Authorization` included, as they go to the same server. A
 * `Location` naming another server or scheme, empty or a fragment alone, repeated with another value, that cannot be
 * read, or leading to a path that a server decoding it could take for another server, leaves the 3xx response as the
 * result. Each request of a chain takes its own place and has its own timeouts. Only the last response comes back: the
 * others are dropped, their `Set-Cookie` included, so that a client keeping cookies leaves max_redirects at 0.
 *
 * A failure to connect is reported as the connector reports it; a failure to send the request or to read its response
 * throws std::system_error from the task, with the stream's error (`std::errc::connection_reset`...), the parser's
 * (`http::error::partial_message`, `http::error::body_too_large`, `http::error::bad_message` past 8 interim
 * responses...), `http::error::too_many_redirects` or `io::error::end_of_stream`. A stop request on the awaiter's
 * context abandons the request, with `std::errc::operation_canceled`, and closes its connection; a timeout does the
 * same, with `std::errc::timed_out` (see transport_options).
 *
 * Copies share their connections. The task copies what it needs: the transport does not have to outlive it.
 *
 * Thread safety: send() and stream() may be called from any thread, concurrently, if the connector allows it.
 */
template <io::Connector Connector = io::network_connector>
class transport {
public:
  using options_type = transport_options;

  /**
   * @brief A transport to `host` (a name, or an address; IPv6 without brackets) on `port`.
   *
   * It counts as `http` for a redirect, whatever the connector does.
   *
   * @throws std::invalid_argument for a host that is neither a name (letters, digits, `-`, `_` and dots), an IPv4 nor
   * an IPv6 address: one with a port (`localhost:8080`), brackets, a zone index, spaces...
   */
  transport(Connector connector, std::string host, std::uint16_t port, transport_options options = {})
      : core_(make_core(std::move(connector), std::move(host), port, false, {}, options)) {}

  /**
   * @brief A transport to the server of `url`, `http[s]://host[:port][/path]` (an IPv6 address in brackets), connecting
   * with a default-constructed connector: by default through the network of the executor that sends
   * (io::network_connector), as quoll::runtime provides it. `https` checks the server as `options.tls` says: by
   * default, against the system's authorities.
   *
   * A path goes before the target of every request, as it is: with `https://api.example.com/v1`, a target `/users` is
   * `/v1/users`, and `/` is `/v1/`, never `/v1`. It is no boundary: a target with dot segments of its own can climb out
   * of it on a server or a proxy resolving them.
   *
   * @code
   * quoll::client::transport<> transport{"https://api.example.com/v1"};
   * @endcode
   *
   * @throws std::invalid_argument for another URL, for a path that would not read as itself (a character RFC 3986 does
   * not allow there, an invalid escape, a dot segment, encoded or not, a NUL, an empty segment), and for `https` with a
   * connector that is no io::TlsConnector.
   */
  explicit transport(std::string_view url, transport_options options = {})
    requires std::default_initializable<Connector>
      : transport(Connector{}, detail::parse_url(url), options) {}

  /**
   * @brief Sends `request` and produces its response.
   *
   * With a `request.body_writer`, its body goes in pieces, in chunks unless a `Content-Length` says how much comes, on
   * a connection of its own: the writer runs once, so that nothing is sent again, and a 3xx response is a result,
   * whatever `max_redirects` says. The response is read meanwhile. Once whole, it stops the writer through its stop
   * token, and write() then throws, so that a server refusing a body has its answer heard; the task ends with the
   * writer's. The answer stands, whatever the writer throws from then on. A server that closes makes write() throw too,
   * and its answer is still the result, if it gave one. What else the writer throws cuts the connection and comes out
   * of this task, as does a body short of its `Content-Length`. `response_timeout` applies to each piece, then to the
   * response.
   *
   * @throws std::invalid_argument here, before the task runs, for a target not in origin form (starting with `/`), and
   * for what http::serialize() rejects, for a `body_writer` with a body too, or over HTTP/1.0 with no `Content-Length`.
   */
  [[nodiscard]] async::task<http::response> send(http::request request) const {
    return detail::send(core_, std::move(request));
  }

  /**
   * @brief Sends `request` and produces its response once its head has come: its body is read in pieces, for what
   * memory cannot or need not hold, up to `limits.max_streamed_body_size`. Connections and retries are as in send() up
   * to the head: from there, a connection that closes is a failure of `body.read()`, as what came is the caller's. A
   * 3xx response is a result, whatever `max_redirects` says, as its body is for the caller to read or drop.
   *
   * With a `request.body_writer`, as in send(), except that the head of the response stops the writer, as the caller
   * reads on its own: a server has to read the whole of the request before it answers, for it all to leave. Over TLS, a
   * write that this cuts short leaves its session unusable: the head is there, and `body.read()` may then fail.
   *
   * Read to its end, or with nothing to read, its connection goes back to the transport. Destroyed before, with every
   * copy of `body`, the connection closes; the last copy goes on the loop of what reads it, as it holds the connection.
   * Reading has a timeout of `response_timeout` per piece. A failure closes the connection, leaving its place to
   * others.
   *
   * @throws std::invalid_argument here, as send() does.
   */
  [[nodiscard]] async::task<http::streamed_response> stream(http::request request) const {
    return detail::stream(core_, std::move(request));
  }

private:
  transport(Connector connector, detail::origin server, transport_options options)
      : core_(make_core(std::move(connector), std::move(server.host), server.port, server.secure,
                        std::move(server.prefix), options)) {
    if (server.secure && !io::TlsConnector<Connector>) {
      throw std::invalid_argument{"quoll::client::transport: this connector cannot open https connections"};
    }
  }

  // The only part that knows the connector.
  [[nodiscard]] static std::shared_ptr<detail::transport_core> make_core(Connector connector, std::string host,
                                                                         std::uint16_t port, bool secure,
                                                                         std::string prefix,
                                                                         transport_options const &options) {
    detail::connector_handle handle{
        std::make_shared<Connector const>(std::move(connector)),
        [](void const *erased, std::string const &to, std::uint16_t on, [[maybe_unused]] bool use_tls,
           [[maybe_unused]] io::tls_client_options const &tls) -> async::task<io::any_stream> {
          auto const &target = *static_cast<Connector const *>(erased);

          if constexpr (io::TlsConnector<Connector>) {
            if (use_tls) {
              return target.connect_tls(to, on, tls);
            }
          }

          return target.connect(to, on);
        }};

    return detail::make_transport_core(std::move(handle), std::move(host), port, secure, std::move(prefix), options);
  }

  std::shared_ptr<detail::transport_core> core_;
};

} // namespace quoll::client

#endif // QUOLL_CLIENT_TRANSPORT_HPP
