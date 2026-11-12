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

#ifndef QUOLL_HTTP_ROUTER_HPP
#define QUOLL_HTTP_ROUTER_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/detail/typed_route.hpp>
#include <quoll/http/next.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/route_match.hpp>
#include <quoll/http/verb.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::http {

/**
 * @brief Maps a method and a path template to a handler `request -> task<response>`, without doing any I/O.
 *
 * Templates are `/users/{id}`: a parameter takes a whole segment, `{name:type}` leaves the type to the caller, and
 * `{name:path}` takes the rest of the path (at least one segment, possibly empty), last. When several routes match a
 * path, the most specific wins, segment by segment: a literal beats a parameter, which beats the rest of the path, so
 * `/users/me` wins over `/users/{id}`. Segments are compared after percent-decoding, literals of the template too.
 * `/users` and `/users/` are different paths. An encoded slash (`%2F`) is a character of its segment: it may be in a
 * `{name}` parameter, but not in the rest of the path, where it would read as a separator.
 *
 * Targets are origin-form (`/users?limit=2`) or absolute-form (`http://host/users`). A HEAD request without a HEAD
 * route goes to the GET route: its response carries the GET body, which whoever writes it for a HEAD request leaves
 * out.
 *
 * A path that matches no route answers 404, a path whose routes have other methods 405 with `Allow`, and another form
 * of target, an invalid escape, a NUL or a dot segment (`.`, `..`, which the router does not resolve) 400. These
 * responses have no body.
 *
 * Middlewares wrap all of this, the 404 / 405 / 400 too, as logs, CORS or a rate limit need them.
 *
 * Thread safety: add routes and middlewares first; dispatch() may then be called from any thread, so that a handler or
 * a middleware with a state of its own (a rate limit) protects it itself. The router outlives
 * the tasks it returns.
 */
class router {
public:
  // std::function rather than std::move_only_function, which Emscripten's libc++ does not have yet: handlers are
  // copyable. By reference, to what the router keeps until the task ends: a handler or a middleware may take them by
  // value or by reference, generic or not, in a coroutine too.
  using handler = std::function<async::task<response>(request &&, route_match &&)>;
  using middleware = std::function<async::task<response>(request &&, next const &)>;

  router();
  router(router const &);
  router(router &&) noexcept;
  router &operator=(router const &);
  router &operator=(router &&) noexcept;
  ~router();

  /**
   * @brief Routes `method` requests on `path` to `target`, under `name`. It takes the request and a route_match, each
   * by value, `const &` or `&&`: what it refers to lives until its task ends.
   * @throws std::invalid_argument for an invalid template, an empty name or handler, or a route this method already has
   * with the same shape (`/users/{id}` and `/users/{uid}`).
   */
  void add(verb method, std::string_view path, std::string name, handler target);

  /**
   * @brief Routes GET requests on `path` to `target`, which takes the parameters of the path, by position:
   *
   * ```cpp
   * router.get("/users/{id}/cards/{card}", [](http::request request, std::int64_t id, std::string card) { ... });
   * router.delete_("/users/{id}", [](std::int64_t id) -> task<http::response> { ... });
   * ```
   *
   * The request is optional, and first. A parameter is a number, a `bool` (`true` or `false`), a `std::string` or a
   * `std::string_view`, and references and views stay valid in a coroutine too, until its task ends. The handler
   * returns `http::response`, or `task<http::response>`; one that takes an http::response_writer, last, `task<>`.
   *
   * Checked at compile time: `path` is a string literal, a valid template, with as many parameters as the handler
   * takes, which must have types (no `auto`). A path made at run time goes through add(), with a route_match.
   *
   * A parameter that cannot be read as its type answers 400. The route is named after `path`, as next::route() says.
   * The handler says the types: the only one a path says is `{name:path}`, for the rest of it. The query is in
   * `request.target`, or in the route_match of add(). A parameter of type http::body_reader, after those of the path,
   * reads the body of the request in pieces, rather than `request.body`, empty for the middlewares too: see
   * body_reader.
   */
  template <class F>
  void get(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::get, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void post(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::post, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void put(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::put, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void patch(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::patch, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void delete_(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::delete_, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void head(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::head, path.text(), std::move(target));
  }

  /** @copydoc get */
  template <class F>
  void options(detail::checked_path<std::type_identity_t<F>> path, F target) {
    add_typed(verb::options, path.text(), std::move(target));
  }

  /**
   * @brief Runs `wrapping` around every request: it answers by awaiting `next`, changing what it likes on the way, or
   * by itself, without it. The first one added is the outermost.
   *
   * Routing is done by then: `next.path()`, `next.route()` and `next.params()` say what it found, and are what to take
   * a security decision on, as `request.target` is as the peer sent it (`/%61dmin` is routed to `/admin`). What a
   * middleware changes in the request reaches the handler, but not what comes of its method and target: the route, its
   * parameters and its query are those that came.
   *
   * What it does after `next` does not run for what `next` throws: catch it to answer a 500 of its own, or the
   * connection answers one without it. A request refused as it is read (400, 413...) reaches no middleware. A response
   * it changes has to stay valid: a body larger than its `Content-Length` answers 500, and a smaller one answers 500
   * too, but to HEAD, or as a 304.
   *
   * It takes the request by value, `const &` or `&&`, and `next` by value or `const &`: what it refers to lives until
   * its task ends.
   *
   * @throws std::invalid_argument for an empty middleware.
   */
  void use(middleware wrapping);

  /**
   * @brief The response of the route `message` matches, from its handler, or the 404 / 405 / 400 above, through the
   * middlewares. Anything a handler or a middleware throws, even before it returns its task, comes out of the task.
   */
  [[nodiscard]] async::task<response> dispatch(request message, body_reader reader = {}) const &QUOLL_LIFETIMEBOUND;

  // A temporary would be gone before the task reads it.
  async::task<response> dispatch(request message, body_reader reader = {}) const && = delete;

  /**
   * @brief Whether the route of a request with this head reads its body in pieces, so that what serves it leaves the
   * body unread, and gives dispatch() what reads it. A request of another method pays nothing for this.
   */
  [[nodiscard]] bool reads_body_in_pieces(request const &head) const;

private:
  friend class next;

  struct entry;

  template <class F>
  void add_typed(verb method, std::string_view path, F target) {
    add_route(method, path, std::string{path}, detail::typed_handler(std::move(target)), detail::reads_body_in_pieces<F>);
  }

  void add_route(verb method, std::string_view path, std::string name, handler target, bool body_in_pieces);

  // What routing finds for a request, without running anything.
  [[nodiscard]] detail::resolved_route resolve(request const &message) const;
  // From the middleware at `position`, or the route past the last one.
  [[nodiscard]] async::task<response>
  run_from(detail::resolved_route const &resolved, std::size_t position, request message) const;

  std::vector<entry> routes_;
  std::vector<middleware> middlewares_;
  bool any_body_in_pieces_ = false;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_ROUTER_HPP
