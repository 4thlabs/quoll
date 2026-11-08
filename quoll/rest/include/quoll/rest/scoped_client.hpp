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

#ifndef QUOLL_REST_SCOPED_CLIENT_HPP
#define QUOLL_REST_SCOPED_CLIENT_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/rest/detail/client_calls.hpp>
#include <quoll/rest/detail/scope_view.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest {

/**
 * @brief The endpoints of `Scope`, an api nested in the client's, with the parameters of the prefixes above them fixed:
 * what client::scope returns.
 *
 * `call<Endpoint>(arguments...)` and `try_call<Endpoint>(arguments...)` are those of the client, without the arguments
 * the scope fixes, for an endpoint of `Scope` only. It keeps a copy of the client as it was when it was made: its
 * transport, which an http::client shares with it, its credentials and its interceptors. Thread safety: as the
 * client's.
 *
 * @code
 * auto repo = github.scope<github::repos::api>("4thlabs", "quoll");    // fixes {owner} and {repo}
 * auto open = co_await repo.call<github::repos::issues>({.state = "open"});
 * @endcode
 */
template <class Client, class Scope>
class scoped_client
    : public detail::client_call_set<
          scoped_client<Client, Scope>, detail::scope_view<typename Client::api_type, Scope>,
          std::make_index_sequence<
              detail::max_arity(typename detail::scope_view<typename Client::api_type, Scope>::routes{}) + 1>> {
  using view = detail::scope_view<typename Client::api_type, Scope>;

public:
  /** @brief The values of the prefixes' parameters it fixes, of the types the endpoints take them as. */
  using prefix_type = typename view::prefix;

  /** @brief The arguments of a call to `Endpoint` through it, as a tuple. */
  template <class Endpoint>
  using arguments = detail::arguments_of<view, Endpoint>;

  scoped_client(Client client, prefix_type prefix) noexcept(std::is_nothrow_move_constructible_v<Client> &&
                                                            std::is_nothrow_move_constructible_v<prefix_type>)
      : client_(std::move(client)), prefix_(std::move(prefix)) {}

  /**
   * @brief A scope of `Inner`, an api nested in this one's, fixing the parameters of the prefixes between them, as
   * client::scope does.
   */
  template <class Inner, class... More>
  [[nodiscard]] scoped_client<Client, Inner> scope(More... more) const {
    using inner = typename scoped_client<Client, Inner>::prefix_type;

    static_assert(std::tuple_size_v<prefix_type> + sizeof...(More) == std::tuple_size_v<inner>,
                  "quoll::rest::scoped_client::scope: as many arguments as the prefixes between the two apis have "
                  "parameters");

    return {client_,
            std::apply([&more...](auto const &...fixed) { return inner{fixed..., std::move(more)...}; }, prefix_)};
  }

  /** @brief The client it calls through. */
  [[nodiscard]] Client const &client() const noexcept QUOLL_LIFETIMEBOUND { return client_; }

  /** @brief The values it fixes. */
  [[nodiscard]] prefix_type const &prefix() const noexcept QUOLL_LIFETIMEBOUND { return prefix_; }

private:
  template <class, class, class>
  friend struct detail::client_calls;

  template <class Endpoint>
  [[nodiscard]] auto call_with(arguments<Endpoint> args) const {
    return client_.template call_with<Endpoint>(std::tuple_cat(prefix_, std::move(args)));
  }

  template <class Endpoint>
  [[nodiscard]] auto try_call_with(arguments<Endpoint> args) const {
    return client_.template try_call_with<Endpoint>(std::tuple_cat(prefix_, std::move(args)));
  }

  Client client_;
  prefix_type prefix_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_SCOPED_CLIENT_HPP
