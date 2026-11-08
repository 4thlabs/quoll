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
#include <quoll/http/verb.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/server/listen_options.hpp>
#include <quoll/server/serve_options.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::http {

namespace detail {

struct server_state;

} // namespace detail

/**
 * @brief Serves routes over connections: the server of a program. It holds its own, added to it, or a copy of a router
 * built apart, for one shared by several servers or built by another part of the program.
 *
 * @code
 * quoll::async::task<> serve_notes(std::uint16_t port) {
 *   quoll::http::server server;
 *
 *   quoll::rest::mount<notes::api>(server, notes_service{});
 *   co_await server.listen(port);
 * }
 * @endcode
 *
 * With options, and a router:
 *
 * @code
 * quoll::http::server simple{{.max_connections = 1000}};
 * quoll::http::server custom{router, {.max_connections = 1000}};
 * @endcode
 *
 * Its routes are closed once it serves, as connections read them: adding one then throws std::logic_error, and a route
 * added to the `router` it copied is not served. A handler with a state of its own is copied with its router, so that
 * two servers of one router share it only through a `std::shared_ptr`.
 *
 * A handler does not capture a copy of its server, which would keep it alive forever, along with all it holds.
 *
 * It may listen on several ports and serve several listeners, each as a task of its own, which ends on its stop
 * request. `max_connections` counts for all of them.
 *
 * A copy is the same server, with the same routes, options and connections. Its tasks share what it holds, so that
 * nothing has to outlive them: not the router it was given, nor the server, as a temporary shows.
 *
 * Thread safety: add routes from one thread, before it serves. Its const functions may then be called from any thread.
 */
class server {
public:
  /** @brief A server with no route yet, serving as `options` says. */
  explicit server(serve_options options = {});

  /** @brief A server of a copy of `router`, serving as `options` says. */
  explicit server(http::router router, serve_options options = {});

  // Copied rather than moved, which is as cheap: one moved from stays usable, rather than hold a null pointer.
  server(server const &) noexcept = default;
  server &operator=(server const &) noexcept = default;
  ~server() = default;

  /** @brief Its router, to read it, or to copy it for an http::local_transport. */
  [[nodiscard]] http::router const &routes() const noexcept QUOLL_LIFETIMEBOUND;

  /**
   * @brief Calls `edit` with its router, for what adds routes to one, as rest::mount does. Connections read it once the
   * server serves: `edit` does not keep the reference.
   *
   * @throws std::logic_error once it serves.
   */
  template <std::invocable<http::router &> Edit>
  void edit_routes(Edit &&edit) {
    std::invoke(std::forward<Edit>(edit), open_routes());
  }

  /** @brief See router::add(). @throws std::logic_error once it serves. */
  void add(verb method, std::string_view path, std::string name, router::handler target) {
    open_routes().add(method, path, std::move(name), std::move(target));
  }

  /** @brief See router::get(). @throws std::logic_error once it serves. */
  template <class F>
  void get(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().get(path, std::move(target));
  }

  /** @brief See router::post(). @throws std::logic_error once it serves. */
  template <class F>
  void post(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().post(path, std::move(target));
  }

  /** @brief See router::put(). @throws std::logic_error once it serves. */
  template <class F>
  void put(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().put(path, std::move(target));
  }

  /** @brief See router::patch(). @throws std::logic_error once it serves. */
  template <class F>
  void patch(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().patch(path, std::move(target));
  }

  /** @brief See router::delete_(). @throws std::logic_error once it serves. */
  template <class F>
  void delete_(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().delete_(path, std::move(target));
  }

  /** @brief See router::head(). @throws std::logic_error once it serves. */
  template <class F>
  void head(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().head(path, std::move(target));
  }

  /** @brief See router::options(). @throws std::logic_error once it serves. */
  template <class F>
  void options(detail::checked_path<std::type_identity_t<F>> path, F target) {
    open_routes().options(path, std::move(target));
  }

  /** @brief See router::use(). @throws std::logic_error once it serves. */
  void use(router::middleware wrapping) { open_routes().use(std::move(wrapping)); }

  /** @brief How many connections it serves now, through all of its listen(), serve() and serve_connection(). */
  [[nodiscard]] std::size_t connections() const noexcept;

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
   * @brief Takes `listener` over, accepts its connections and serves each one, concurrently, on the executor the task
   * runs on: the connections run where the accept loop does. To know the port the system picked, or that it is ready,
   * ask the listener first:
   *
   * @code
   * auto listener = co_await quoll::io::listen(0);
   * auto const port = listener.local_port();
   *
   * co_await server.serve(std::move(listener));
   * @endcode
   *
   * A stop request is graceful: it ends the accept loop, closes the idle connections, and lets the requests under way
   * finish, within `shutdown_timeout`, past which they are stopped.
   *
   * Accepting stops with `std::errc::operation_canceled`. Another accept failure goes to `on_accept_error`. By default
   * it is skipped when it concerns one connection only, retried after 100 ms when the network is in trouble or the
   * system ran out of something (file descriptors, memory) and it has a timer to wait with, and rethrown otherwise.
   *
   * However the accept loop ends (a stop request, a failure, an exception from `on_accept_error`), the connections it
   * spawned end as above, and the task completes once they have ended, then rethrows the failure if there was one.
   */
  [[nodiscard]] async::task<> serve(io::any_listener listener) const;

  /** @brief serve(), for any listener. */
  template <io::Listener Listener>
    requires(!std::same_as<Listener, io::any_listener>)
  [[nodiscard]] async::task<> serve(Listener listener) const {
    return serve(io::any_listener{std::move(listener)});
  }

  /**
   * @brief serve() with each connection as a task of its own on `executor`: a thread pool or a strand, for instance,
   * while the accept loop stays on its I/O loop. serve() alone puts each on a strand of its own where the executor
   * runs several threads; this one keeps them all on `executor`. An executor without a timer, as a thread pool has
   * none, leaves the timeouts of `connection` without effect, and a slow peer can hold its connection.
   */
  template <async::Executor Executor, io::Listener Listener>
  [[nodiscard]] async::task<> serve(Executor executor, Listener listener) const {
    return serve_on(async::any_executor{executor}, io::any_listener{std::move(listener)});
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
   * - An answer its peer would read as something else is replaced, and `connection.on_handler_error` told: a 2xx to
   * CONNECT answers 501, and a 1xx, or a `Content-Length` with no body, but to HEAD or as a 304, answers 500.
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
  [[nodiscard]] http::router &open_routes();
  [[nodiscard]] async::task<> serve_on(async::any_executor executor, io::any_listener listener) const;

  std::shared_ptr<detail::server_state> state_;
};

} // namespace quoll::http

#endif // QUOLL_SERVER_SERVER_HPP
