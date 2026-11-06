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

#ifndef QUOLL_SERVER_SERVER_HPP
#define QUOLL_SERVER_SERVER_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/server/listen_options.hpp>
#include <quoll/server/serve_options.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <utility>

namespace quoll::http {

namespace detail {

// A listener, whichever it is. Non-owning: the listener outlives the task.
struct listener_ref {
  void *listener;
  async::task<io::any_stream> (*accept)(void *listener);

  template <io::Listener Listener>
  explicit listener_ref(Listener &target) noexcept
      : listener(&target),
        accept([](void *erased) -> async::task<io::any_stream> { return static_cast<Listener *>(erased)->accept(); }) {}
};

[[nodiscard]] async::task<> serve_accepting(async::any_executor executor, listener_ref listener,
                                            std::shared_ptr<http::router const> router, serve_options options);
[[nodiscard]] async::task<> serve_accepting_here(listener_ref listener, std::shared_ptr<http::router const> router,
                                                 serve_options options);

} // namespace detail

/**
 * @brief Serves a router: the server of a program.
 *
 * @code
 * quoll::async::task<> serve_notes(std::uint16_t port) {
 *   quoll::http::router router;
 *
 *   quoll::rest::mount<notes::api>(router, notes_service{});
 *
 *   quoll::http::server server{router};
 *
 *   co_await server.listen(port);
 * }
 * @endcode
 *
 * It keeps its own copy of the router, which each of its tasks shares, so that nothing has to outlive them but a
 * listener given to serve(). Add the routes first, as one added to `router` afterwards is not served. A handler with a
 * state of its own is copied along: two servers of one router share it only through a `std::shared_ptr`.
 *
 * Each call copies the options, so that a limit such as `max_connections` counts for that call.
 *
 * Thread safety: its const functions may be called from any thread.
 */
class server {
public:
  /** @brief A server of a copy of `router`, serving as `options` says. */
  explicit server(http::router router, serve_options options = {});

  // Copied rather than moved, which is as cheap: one moved from stays usable, rather than hold a null pointer.
  server(server const &) = default;
  server &operator=(server const &) = default;
  ~server() = default;

  /**
   * @brief Listens on `port` and serves there, until a stop request.
   *
   * Listens through the network of the executor the task runs on (io::listen), a runtime's: spawned with the runtime's
   * stop token, it stops on Ctrl-C. Throws std::system_error when the address cannot be bound or the certificate
   * loaded, and fails like serve() otherwise. The task completes once its connections have ended.
   *
   * Over TLS:
   *
   * @code
   * co_await server.listen(8443, {.tls = {.certificate_file = "cert.pem", .private_key_file = "key.pem"}});
   * @endcode
   */
  [[nodiscard]] async::task<> listen(std::uint16_t port, listen_options where = {}) const;

  /**
   * @brief Accepts the connections of `listener` and serves each one, concurrently, on the executor the task runs on:
   * the connections run where the accept loop does.
   *
   * A stop request is graceful: it ends the accept loop, closes the idle connections, and lets the requests under way
   * finish, within `shutdown_timeout`, past which they are stopped.
   *
   * Accepting stops with `std::errc::operation_canceled`: a stop request, or the application closing its listener.
   * Another accept failure goes to `on_accept_error`. By default it is skipped when it concerns one connection only,
   * retried after 100 ms when the system ran out of something (file descriptors, memory) and it has a timer to wait
   * with, and rethrown otherwise; a rethrow ends the accept loop, not the connections already served.
   *
   * However the accept loop ends (a stop request, a closed listener, a failure, an exception from `on_accept_error`),
   * the connections it spawned end as above, and the task completes once they have ended, then rethrows the failure if
   * there was one: `listener` needs to outlive the task only.
   */
  template <io::Listener Listener>
  [[nodiscard]] async::task<> serve(Listener &listener QUOLL_LIFETIMEBOUND) const {
    return detail::serve_accepting_here(detail::listener_ref{listener}, router_, options_);
  }

  /**
   * @brief serve() with each connection as a task of its own on `executor`: a thread pool or a strand, for instance,
   * while the accept loop stays on its I/O loop. An executor without a timer, as a thread pool has none, leaves the
   * timeouts of `connection` without effect, and a slow peer can hold its connection.
   */
  template <async::Executor Executor, io::Listener Listener>
  [[nodiscard]] async::task<> serve(Executor executor, Listener &listener QUOLL_LIFETIMEBOUND) const {
    return detail::serve_accepting(async::any_executor{std::move(executor)}, detail::listener_ref{listener}, router_,
                                   options_);
  }

  /**
   * @brief Serves the requests of one connection with the router, one after the other, until the connection ends.
   *
   * Each request goes to the router; its response is written back before the next request is read (pipelined requests
   * wait in the connection's buffer). The connection ends when the peer closes it, after a response to a request that
   * does not keep it alive (`Connection: close`, HTTP/1.0 without keep-alive, an upgrade, which is not served), after a
   * response whose handler set `Connection: close`, on a stream error, on `connection.drain`, gently, or on a stop
   * request, which cancels whatever is pending: a read, a handler's own operations, a write. Timeouts end it too (see
   * connection_options): a connection idle for too long, a request taking too long to arrive (408), a response taking
   * too long to leave.
   *
   * - A request that is not valid HTTP answers 400, one with too large a header 431, too large a body 413, as soon as
   * its head announces it, an unknown method 501, an `Expect` field asking for anything but `100-continue` 417, and
   * ends the connection.
   * - A request with `Expect: 100-continue` whose body has not come gets `100 Continue`, then the body is read.
   * - A handler or a middleware that throws answers 500 (and is reported to `connection.on_handler_error`), as does a
   * response that cannot be serialized.
   * - The response to HEAD carries no body: it announces the length of the one the GET route produced.
   *
   * Ending a connection stops sending first, with `stream.shutdown_send()`: a TLS peer gets its closing message. After
   * the last response, what the peer still sends is then read and dropped, as `connection.lingering_timeout` and
   * `connection.lingering_time` say, unless `connection.drain` or a stop request ends it.
   *
   * Owns `stream`.
   */
  [[nodiscard]] async::task<> serve_connection(io::any_stream stream) const;

  /** @brief serve_connection() on a stream an io::any_stream can hold. */
  template <io::ByteStream Stream>
    requires std::constructible_from<io::any_stream, Stream>
  [[nodiscard]] async::task<> serve_connection(Stream stream) const {
    return serve_connection(io::any_stream{std::move(stream)});
  }

private:
  std::shared_ptr<http::router const> router_;
  serve_options options_;
};

} // namespace quoll::http

#endif // QUOLL_SERVER_SERVER_HPP
