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

#ifndef QUOLL_REST_SERVICE_HPP
#define QUOLL_REST_SERVICE_HPP

#include <quoll/rest/detail/handled_route.hpp>

#include <utility>

namespace quoll::rest {

/**
 * @brief A service made of handlers, one a route, each taking its route first, then the route's parameters: defined
 * once, mounted elsewhere.
 *
 * @code
 * auto const service = rest::service(
 *     [&store](notes::get, std::int64_t id) -> note { return store.find(id); },
 *     [&store](notes::create, new_note made) { return store.add(std::move(made)); });
 *
 * rest::mount<api>(router, service);
 * @endcode
 *
 * The handlers' call operators, all of them, as a struct of `operator()` overloads written by hand: the two mount the
 * same way, and mix in one mount(). Each route of the api has one handler, of its parameters exactly: one missing or
 * two do not compile. Moved into what the router keeps, it lives as long as it: what a handler captures by reference
 * outlives the router.
 */
template <class... Handlers>
class service : public Handlers... {
  // Two would make its call ambiguous, which mount() would tell as no handler at all.
  static_assert(
      detail::distinct_handled_routes(detail::type_list<typename detail::handled_route<Handlers>::type...>{}),
      "quoll::rest::service: two handlers take the same route"
  );

public:
  /** @brief Made of `handlers`, moved in. */
  explicit service(Handlers... handlers) : Handlers(std::move(handlers))... {}

  using Handlers::operator()...;
};

template <class... Handlers>
service(Handlers...) -> service<Handlers...>;

} // namespace quoll::rest

#endif // QUOLL_REST_SERVICE_HPP
