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

#include <quoll/async/task.hpp>
#include <quoll/http/detail/route_pattern.hpp>
#include <quoll/http/detail/router_responses.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http {

/** @brief What a route matched in a request. */
struct route_match {
  /** @brief The name the route was added with. Valid as long as the router. */
  std::string_view name;
  /** @brief The path parameters, percent-decoded, in the order of the template. */
  std::vector<std::string> params;
  /** @brief The query string, after `?` and still encoded; empty without one. */
  std::string query;
};

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
 * Thread safety: add routes first; dispatch() may then be called from any thread. The router outlives the tasks it
 * returns.
 */
class router {
public:
  // std::function rather than std::move_only_function, which Emscripten's libc++ does not have yet: handlers are
  // copyable.
  using handler = std::function<async::task<response>(request, route_match)>;

  /**
   * @brief Routes `method` requests on `path` to `target`, under `name`.
   * @throws std::invalid_argument for an invalid template, an empty handler, or a route this method already has with
   * the same shape (`/users/{id}` and `/users/{uid}`).
   */
  void add(verb method, std::string_view path, std::string name, handler target) {
    if (!target) {
      throw std::invalid_argument{"quoll::http::router: empty handler"};
    }

    auto segments = detail::compile_route(path);

    for (auto const &route : routes_) {
      if (route.method == method && detail::same_shape(route.segments, segments)) {
        throw std::invalid_argument{"quoll::http::router: " + route.name + " already routes this method and path"};
      }
    }

    routes_.push_back(entry{method, std::move(segments), std::move(name), std::move(target)});
  }

  /**
   * @brief The response of the route `message` matches, from its handler, or the 404 / 405 / 400 above. Anything the
   * handler throws, even before it returns its task, comes out of the task.
   */
  [[nodiscard]] async::task<response> dispatch(request message) const {
    auto const target = detail::split_target(message.target);
    auto const segments = target ? detail::decode_segments(target->path) : std::nullopt;

    if (!segments) {
      co_return detail::status_response(status::bad_request);
    }

    entry const *chosen = nullptr;
    entry const *get_route = nullptr;
    std::vector<verb> allowed;

    for (auto const &route : routes_) {
      if (!detail::matches(route.segments, *segments)) {
        continue;
      }

      if (route.method == message.method) {
        chosen = more_specific(chosen, route);
        continue;
      }

      if (route.method == verb::get) {
        get_route = more_specific(get_route, route);
      }

      allow(allowed, route.method);
    }

    // HEAD is served by the GET route, as RFC 9110 asks of a general-purpose server.
    if (chosen == nullptr && message.method == verb::head) {
      chosen = get_route;
    }

    if (chosen != nullptr) {
      route_match match{chosen->name, detail::extract_params(chosen->segments, *segments), std::string{target->query}};

      co_return co_await chosen->target(std::move(message), std::move(match));
    }

    if (!allowed.empty()) {
      auto rejected = detail::status_response(status::method_not_allowed);

      rejected.fields.add("Allow", detail::join_methods(allowed));
      co_return rejected;
    }

    co_return detail::status_response(status::not_found);
  }

private:
  struct entry {
    verb method;
    std::vector<detail::route_segment> segments;
    std::string name;
    handler target;
  };

  // The more specific of two matching routes, segment by segment; the first added on a tie.
  [[nodiscard]] static entry const *more_specific(entry const *current, entry const &candidate) {
    auto const kind = &detail::route_segment::kind;

    if (current == nullptr ||
        std::ranges::lexicographical_compare(current->segments, candidate.segments, {}, kind, kind)) {
      return &candidate;
    }

    return current;
  }

  // Adds `method` to the methods of `Allow`, and HEAD with GET.
  static void allow(std::vector<verb> &allowed, verb method) {
    auto const add_once = [&allowed](verb each) {
      if (std::ranges::find(allowed, each) == allowed.end()) {
        allowed.push_back(each);
      }
    };

    add_once(method);

    if (method == verb::get) {
      add_once(verb::head);
    }
  }

  std::vector<entry> routes_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_ROUTER_HPP
