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

#ifndef QUOLL_CLIENT_CLIENT_HPP
#define QUOLL_CLIENT_CLIENT_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/client_options.hpp>
#include <quoll/client/detail/client_backend.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/http/detail/number.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/http/streaming_transport.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::http {

/**
 * @brief HttpTransport speaking HTTP/1.1 to one server, over the byte streams of a connector: by default the network
 * of the executor that sends (io::network_connector), as quoll::runtime provides it.
 *
 * Each request goes to `host` on `port`, with a `Host` field naming them unless the request has one. Connections are
 * kept open once their response is read (keep-alive), up to `max_idle_connections`, and the next requests reuse them;
 * a request waits for no other, unless client_options::max_connections says so, and opens a new connection when none
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
 * Redirects are not followed unless client_options::max_redirects says so: a 3xx response is a result like the
 * others. With it, a 301, 302, 303, 307 or 308 response whose `Location` names the client's scheme and the host and
 * port of the request's `Host` field, by default the client's, sends the request again there, its target being what
 * `Location` says, whatever the path of the client's URL. 307 and 308 keep the method. 303 makes it a GET, but for a
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
 * same, with `std::errc::timed_out` (see client_options).
 *
 * Copies share their connections. The task copies what it needs: the client does not have to outlive it.
 *
 * In a browser, a Worker or Node, built with Emscripten, a client from a URL goes through the JavaScript fetch(), as
 * the executor offers it, and the same code does the same thing, but for what fetch keeps to itself:
 *
 * - A URL may be relative: empty or a path, from the page's origin, or `.` or `./path`, from the page itself, for one
 *   served under a path of its own. Elsewhere it throws std::invalid_argument. On an executor that does not offer
 * fetch, a request throws std::logic_error, as one without a network does natively.
 * - What fetch refuses throws std::invalid_argument, before the task runs: a GET or a HEAD with a body or a
 *   `body_writer`, and the methods CONNECT and TRACE.
 * - `max_connections`, `max_idle_connections`, `max_redirects`, `connect_timeout` and `tls` have no effect, as the
 * browser decides. It follows redirects itself, possibly to another origin, and always checks certificates. `limits`
 * and `response_timeout` apply: past a limit, the fetch is aborted.
 * - The fields fetch manages itself (Host, Content-Length, Connection and the other hop-by-hop ones) are not sent, nor
 *   are trailers. A `request.body_writer` runs to its end before the fetch, and goes out whole.
 * - fetch decodes the body, so that the fields describing the bytes on the wire (Content-Encoding, Content-Length,
 *   Transfer-Encoding) are dropped, the others are named in lower case, repeated ones are combined, and the version is
 *   not known (`http_1_1`). A browser does not expose a 3xx response.
 * - A failure to get a response (network error, refused connection, CORS) throws std::system_error, with
 *   `std::errc::io_error` and fetch's message. The task resumes on its awaiter's executor, on the event loop.
 * - `trailers` of a streamed response is not null and stays empty.
 *
 * Thread safety: send(), stream() and the shortcuts over send() may be called from any thread, concurrently, if the
 * connector allows it. In a browser, use it, the bodies it gives included, from the thread running the event loop.
 */
class client {
public:
  using options_type = client_options;

  /**
   * @brief A client of the server at `url`, `http[s]://host[:port][/path]` (an IPv6 address in brackets), connecting
   * through the network of the executor that sends (io::network_connector), as quoll::runtime provides it. `https`
   * checks the server as `options.tls` says: by default, against the system's authorities.
   *
   * A path goes before the target of every request, as it is: with `https://api.example.com/v1`, a target `/users` is
   * `/v1/users`, and `/` is `/v1/`, never `/v1`. It is no boundary: a target with dot segments of its own can climb out
   * of it on a server or a proxy resolving them.
   *
   * @code
   * quoll::http::client client{"https://api.example.com/v1"};
   * @endcode
   *
   * @throws std::invalid_argument for another URL, but for one relative to the page in a browser, and for a path that
   * would not read as itself (a character RFC 3986 does not allow there, an invalid escape, a dot segment, encoded or
   * not, a NUL, an empty segment, encoded or not, what a proxy could read as another server), and for a host with an
   * empty label or port 0.
   */
  explicit client(std::string_view url, client_options options = {});

  // Copied rather than moved, which is as cheap: one moved from stays usable, rather than hold a null pointer.
  client(client const &) noexcept = default;
  client &operator=(client const &) noexcept = default;
  ~client() = default;

  /**
   * @brief A client of the server at `url`, as above, connecting through `connector`: asio's on a context of its own, a
   * stream in memory in a test...
   *
   * @throws std::invalid_argument as above, and for `https` with a connector that is no io::TlsConnector.
   */
  template <io::Connector Connector>
  client(Connector connector, std::string_view url, client_options options = {})
      : client(std::move(connector), detail::parse_url(url), options) {}

  /**
   * @brief A client of `host` (a name, or an address; IPv6 without brackets) on `port`, connecting through `connector`.
   *
   * It counts as `http` for a redirect, whatever the connector does.
   *
   * `Port` is any integer rather than `std::uint16_t`, so that braces after a URL always mean its options: `{}` would
   * otherwise convert better to a port, and `{connector, "http://example.com", {}}` read its URL as a host.
   *
   * @throws std::invalid_argument for a port out of 0 to 65535, which is the connector's to read, and for a host that
   * is neither a name (letters, digits,
   * `-`, `_` and dots), an IPv4 nor an IPv6 address: one with a port (`localhost:8080`), brackets, a zone index,
   * spaces...
   */
  template <io::Connector Connector, std::integral Port>
    requires(!http::detail::is_any_of<Port, bool, char, wchar_t, char8_t, char16_t, char32_t>)
  client(Connector connector, std::string host, Port port, client_options options = {})
      : backend_(make_backend(std::move(connector), std::move(host), checked_port(port), false, {}, options)) {}

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
    return backend_.operations->send(backend_.state, std::move(request));
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
   * Read to its end, or with nothing to read, its connection goes back to the client. Destroyed before, with every
   * copy of `body`, the connection closes; the last copy goes on the loop of what reads it, as it holds the connection.
   * Reading has a timeout of `response_timeout` per piece. A failure closes the connection, leaving its place to
   * others.
   *
   * @throws std::invalid_argument here, as send() does.
   */
  [[nodiscard]] async::task<http::streamed_response> stream(http::request request) const {
    return backend_.operations->stream(backend_.state, std::move(request));
  }

  /**
   * @brief Sends a GET to `target`, as send() does. For fields of its own, send() takes a request, as it does for a
   * request body in pieces, and stream() reads a response in pieces.
   *
   * @code
   * auto const users = co_await client.get("/users?limit=20");
   * @endcode
   *
   * @throws std::invalid_argument here, as send() does.
   */
  [[nodiscard]] async::task<http::response> get(std::string target) const;

  /** @brief Sends a HEAD to `target`: see get(). */
  [[nodiscard]] async::task<http::response> head(std::string target) const;

  /** @brief Sends a DELETE to `target`: see get(). */
  [[nodiscard]] async::task<http::response> delete_(std::string target) const;

  /**
   * @brief Sends a POST of `body` to `target`, as send() does, with `content_type` as its `Content-Type`, if not empty.
   *
   * @code
   * auto const created = co_await client.post("/users?notify=true", R"({"name":"Ada"})", "application/json");
   * @endcode
   *
   * @throws std::invalid_argument here, as send() does.
   */
  [[nodiscard]] async::task<http::response> post(std::string target, std::string body,
                                                 std::string_view content_type = {}) const;

  /** @brief Sends a PUT of `body` to `target`: see post(). */
  [[nodiscard]] async::task<http::response> put(std::string target, std::string body,
                                                std::string_view content_type = {}) const;

  /** @brief Sends a PATCH of `body` to `target`: see post(). */
  [[nodiscard]] async::task<http::response> patch(std::string target, std::string body,
                                                  std::string_view content_type = {}) const;

private:
  template <io::Connector Connector>
  client(Connector connector, detail::origin server, client_options const &options)
      : backend_(make_backend(std::move(connector), std::move(server.host), server.port, server.secure,
                              std::move(server.prefix), options)) {
    if (server.secure && !io::TlsConnector<Connector>) {
      throw std::invalid_argument{"quoll::http::client: this connector cannot open https connections"};
    }
  }

  template <std::integral Port>
  [[nodiscard]] static std::uint16_t checked_port(Port port) {
    if (!std::in_range<std::uint16_t>(port)) {
      throw std::invalid_argument{"quoll::http::client: the port is out of range"};
    }

    return static_cast<std::uint16_t>(port);
  }

  // The only part that knows the connector.
  template <io::Connector Connector>
  [[nodiscard]] static detail::client_backend make_backend(Connector connector, std::string host, std::uint16_t port,
                                                           bool secure, std::string prefix,
                                                           client_options const &options) {

    return detail::make_client_backend(detail::make_connector_handle(std::move(connector)), std::move(host), port,
                                       secure, std::move(prefix), options);
  }

  detail::client_backend backend_;
};

static_assert(HttpTransport<client>);
static_assert(StreamingTransport<client>);

} // namespace quoll::http

#endif // QUOLL_CLIENT_CLIENT_HPP
