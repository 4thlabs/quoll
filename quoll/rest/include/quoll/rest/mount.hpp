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

#ifndef QUOLL_REST_MOUNT_HPP
#define QUOLL_REST_MOUNT_HPP

#include <quoll/http/request.hpp>
#include <quoll/http/router.hpp>
#include <quoll/rest/detail/handled_route.hpp>
#include <quoll/rest/detail/routes.hpp>
#include <quoll/rest/detail/serve.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/validators.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest {

namespace detail {

template <class T>
struct is_validator_set : std::false_type {};

template <class... Validators>
struct is_validator_set<validator_set<Validators...>> : std::true_type {};

// The positions of the arguments of mount() that are services, and of its validators (the size if none).
template <class... Args>
struct mount_arguments {
  static constexpr std::array<bool, sizeof...(Args)> validators{is_validator_set<Args>::value...};
  static constexpr std::size_t validator_count = count_true(validators);
  static constexpr std::size_t validator_position = first_true(validators);
  static constexpr std::size_t service_count = sizeof...(Args) - validator_count;

  static constexpr auto service_positions = [] {
    std::array<std::size_t, service_count> positions{};
    std::size_t next = 0;

    for (std::size_t index = 0; index < validators.size(); ++index) {
      if (!validators[index]) {
        positions[next++] = index;
      }
    }

    return positions;
  }();
};

template <class... Args>
[[nodiscard]] auto make_mounted(Args &&...args) {
  using layout = mount_arguments<std::decay_t<Args>...>;
  using decayed = std::tuple<std::decay_t<Args>...>;

  static_assert(layout::validator_count <= 1, "quoll::rest::mount: more than one validators()");

  auto all = std::forward_as_tuple(std::forward<Args>(args)...);

  auto validators = [&] {
    if constexpr (layout::validator_count == 1) {
      return std::forward<std::tuple_element_t<layout::validator_position, decltype(all)>>(
          std::get<layout::validator_position>(all)
      );
    } else {
      return validator_set<>{};
    }
  }();

  return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    using state = mounted<decltype(validators), std::tuple_element_t<layout::service_positions[Index], decayed>...>;

    return std::make_shared<state>(
        std::move(validators),
        std::tuple{std::forward<std::tuple_element_t<layout::service_positions[Index], decltype(all)>>(
            std::get<layout::service_positions[Index]>(all)
        )...}
    );
  }(std::make_index_sequence<layout::service_count>{});
}

// Whether each route these handlers take is one of `Routes`: a handler of another api would be served by nothing.
template <class Routes, class... Handled>
[[nodiscard]] consteval bool handles_own_routes(type_list<Handled...>) {
  return ((std::is_void_v<Handled> || matching_routes<Handled, Routes>::type::size != 0) && ...);
}

} // namespace detail

/**
 * @brief Serves the endpoints of `Api` on `router`, with `services` and, optionally, a validators() set.
 *
 * A service is an overload set: one `operator()(Endpoint, arguments...)` per endpoint, the arguments being those of
 * the endpoint (path parameters, then its parts in order, then the inherited ones), by value or `const&`. It returns
 * the body of the endpoint's first returns<>, a reply<Endpoint>, or nothing (204, or a first response without body),
 * as is or in a task<>. For an endpoint with events<T>, it takes an rest::event_sink<T> by reference after its
 * arguments, returns a task<>, and sends its events through it, after the router has answered: an http_error before the
 * stream starts (open() or the first event) answers as from any service, after it the stream is cut. It runs as the
 * response's `late_head_writer`: a middleware that sets a `body_writer` of its own replaces it, and the service is not
 * called.
 *
 * Each endpoint has exactly one service, and each security scheme of the api a validator: otherwise mount() does not
 * compile.
 *
 * For each request: credentials (401 if missing or rejected, with the `WWW-Authenticate` challenge of the scheme:
 * `Bearer`, or `ApiKey header="X-Api-Key"` / `ApiKey query="key"` for keys, which have no registered scheme), then
 * the arguments (400 for an invalid or missing one, 415 for a body of another media type), then the service. A
 * service or a validator throwing http_error with an error status answers it. These errors are
 * application/problem+json (problem). Anything else it throws goes on as from any handler: a connection answers 500,
 * with nothing of it, and tells `on_handler_error`, a stop or a timeout reaches what waits for it, and over a
 * http::local_transport or a direct dispatch it reaches the caller. A middleware of the router runs outside of this:
 * what it throws is no http_error, and it answers a problem by returning a response.
 *
 * Adds all the routes or none: @throws std::invalid_argument, leaving `router` as it was, for a route it already has.
 *
 * Services and validators are moved into storage shared by the routes, which lives as long as the router, and as long
 * as a stream of events<> under way; they are called from the thread awaiting the dispatch, concurrently if the router
 * is dispatched concurrently, but for the service of events<>, which runs as the response's writer, on the thread of
 * the connection that writes it.
 */
template <class Api, class... Args>
void mount(http::router &router, Args &&...args) {
  static_assert(
      detail::handles_own_routes<typename Api::routes>(
          detail::concat_t<detail::type_list<>, typename detail::handled_routes<std::decay_t<Args>>::type...>{}
      ),
      "quoll::rest::mount: a handler takes a route this api does not have"
  );

  auto state = detail::make_mounted(std::forward<Args>(args)...);
  // All the routes or none: on a route the router refuses (one it already has), `router` is left as it was.
  auto staged = router;

  [&]<class... Routes>(detail::type_list<Routes...>) {
    (staged.add(
         Routes::method, Routes::path, std::string{Routes::name},
         [state](http::request request, http::route_match match) {
           return detail::serve<Routes>(state, std::move(request), std::move(match));
         }
     ),
     ...);
  }(typename Api::routes{});

  router = std::move(staged);
}

/** @brief mount() on what holds a router of its own, as an http::server. */
template <class Api, class Target, class... Args>
  requires requires(Target &target) { target.edit_routes([](http::router &) {}); }
void mount(Target &target, Args &&...args) {
  target.edit_routes([&](http::router &router) { mount<Api>(router, std::forward<Args>(args)...); });
}

} // namespace quoll::rest

#endif // QUOLL_REST_MOUNT_HPP
