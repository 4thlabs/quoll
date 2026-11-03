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

#ifndef QUOLL_REST_CLIENT_HPP
#define QUOLL_REST_CLIENT_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/transport.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/rest/call_error.hpp>
#include <quoll/rest/detail/client_calls.hpp>
#include <quoll/rest/detail/client_request.hpp>
#include <quoll/rest/detail/client_response.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <concepts>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest {

/** @brief What try_call<Endpoint> produces: one of the endpoint's declared responses, or an unexpected_response. */
template <class Endpoint>
using result = detail::call_result<Endpoint>;

/**
 * @brief Calls the endpoints of `Api` through `Transport` (an http::HttpTransport): a connection natively
 * (quoll::client::transport, the default), fetch in a browser, a Worker or Node, a router of the same program
 * (http::local_transport). The api may be a third-party
 * one: its contract is all a client needs.
 *
 * - `call<Endpoint>(arguments...)` returns a task of the body of the endpoint's first returns<> (nothing for an
 *   endpoint without one, or whose first response has no body). Any other outcome throws call_error from the task:
 *   another declared response (read it with try_call), an undeclared status (its problem detail, if any), a body that
 *   does not decode.
 * - `try_call<Endpoint>(arguments...)` returns a task of result<Endpoint>: a reply<Endpoint> holding the declared
 *   response received, or an unexpected_response.
 * - Transport failures (network, cancellation) are exceptions from both tasks (std::system_error).
 *
 * The arguments are those of the endpoint, in its order: path parameters (prefixes first), then the argument parts,
 * then the inherited ones; their types come from the endpoint, so braced lists convert (`{.limit = 20}`). They are
 * encoded into the request by the call: an invalid one throws std::invalid_argument there, before the task runs (a
 * path parameter that is empty or a dot segment, a header value the server would read differently).
 *
 * The task does not refer to the client: the request is built by the call, and the transport's task owns what it
 * needs. Thread safety: none (credentials() changes the client); calls on a const client may be concurrent.
 */
template <class Api, http::HttpTransport Transport = ::quoll::client::transport<>>
class client : public detail::client_call_set<client<Api, Transport>, Api,
                                              std::make_index_sequence<detail::max_arity(typename Api::routes{}) + 1>> {
public:
  /** @brief The arguments of a call to `Endpoint`, as a tuple. */
  template <class Endpoint>
  using arguments = detail::arguments_of<Api, Endpoint>;

  explicit client(Transport transport) noexcept(std::is_nothrow_move_constructible_v<Transport>)
      : transport_(std::move(transport)) {}

  /**
   * @brief A client of the server at `base_url`, through a transport built from it: by default a connection over the
   * network of the executor that calls (see quoll::client::transport, sync_wait), fetch_transport in a browser.
   *
   * @code
   * quoll::rest::client<bank::v1> bank{"http://127.0.0.1:8080"};
   * @endcode
   */
  explicit client(std::string base_url)
    requires std::constructible_from<Transport, std::string>
      : transport_(std::move(base_url)) {}

  /** @brief The credential sent, from now on, to the endpoints that require `Scheme`. */
  template <class Scheme>
  void credentials(std::string value) {
    static_assert(detail::uses_scheme<Scheme>(typename Api::routes{}), "quoll::rest::client: the api has no endpoint "
                                                                       "that requires this scheme");

    credentials_.insert_or_assign(std::string{detail::scheme_traits<Scheme>::key.view()}, std::move(value));
  }

  [[nodiscard]] Transport const &transport() const noexcept { return transport_; }

private:
  template <class, class, class>
  friend struct detail::client_calls;

  template <class Endpoint>
  [[nodiscard]] async::task<typename detail::call_value<Endpoint>::type> call_with(arguments<Endpoint> args) const {
    using route = typename Api::template route<Endpoint>;

    return detail::receive_value<route>(transport_.send(detail::build_request<route>(args, credentials_)));
  }

  template <class Endpoint>
  [[nodiscard]] async::task<result<Endpoint>> try_call_with(arguments<Endpoint> args) const {
    using route = typename Api::template route<Endpoint>;

    return detail::receive<route>(transport_.send(detail::build_request<route>(args, credentials_)));
  }

  Transport transport_;
  detail::credential_map credentials_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_CLIENT_HPP
