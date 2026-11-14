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

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client/client.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/streaming_transport.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/rest/detail/client_calls.hpp>
#include <quoll/rest/detail/client_events.hpp>
#include <quoll/rest/detail/client_request.hpp>
#include <quoll/rest/detail/client_response.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/next.hpp>

#include <concepts>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest {

/**
 * @brief Calls the routes of `Api` through `Transport` (an http::HttpTransport): a connection natively
 * (quoll::http::client, the default), fetch in a browser, a Worker or Node, a router of the same program
 * (http::local_transport). The api may be a third-party
 * one: its contract is all a client needs.
 *
 * - `call<Route>(arguments...)` returns a task of the value of the route's result (nothing for a void one). Any other
 *   outcome throws an http::http_error received from the task: another status (its problem detail, if any, as
 *   what()), a body that does not decode. Its body_as() reads a third-party API's own shape of its errors.
 * - `try_call<Route>(arguments...)` returns a task of `std::expected<T, http::http_error>`: the value, or that error.
 * - `subscribe<Route>(arguments...)`, for a route of events<T>, returns a task of rest::event_stream<T>, once
 *   the head of a `text/event-stream` has come: its events as they come. Any other answer throws an http::http_error
 *   received from the task, as from call, with its body read up to 64 KiB. It needs a transport that gives a response
 *   as its head comes (`stream()`: http::client, not http::local_transport), and it goes straight through it,
 *   credentials applied.
 *   Interceptors take whole responses: on a client with any, it throws std::logic_error at the call, rather than go
 *   out without what they add.
 * - Transport failures (network, cancellation) are exceptions from the tasks (std::system_error). Over a
 *   http::local_transport, so is what a service throws, as there is no connection to answer 500.
 *
 * The arguments are those of the route, in its order: its path's and its query's parameters, then its headers and its
 * body, then what its groups add; their types come from the route, so braced lists convert (`{.limit = 20}`). They are
 * encoded into the request by the call: an invalid one throws std::invalid_argument there, before the task runs (a
 * path parameter that is empty or holds a NUL or a dot segment, the rest of a path starting with `/`, a query parameter
 * holding a NUL, a number that is not finite, a header value or a credential the server would read differently).
 *
 * Interceptors added with use() wrap what it sends, for call and try_call alike: a retry (rest::retry), a token
 * refreshed on a 401.
 *
 * The task does not refer to the client: the request is built by the call, and the task of a transport owns what it
 * needs, as http::HttpTransport asks, as do the interceptors'. Thread safety: none (credentials() and use() change the
 * client); calls on a const client may be concurrent.
 */
template <class Api, http::HttpTransport Transport = ::quoll::http::client>
class client
    : public detail::client_call_set<
          client<Api, Transport>, Api, std::make_index_sequence<detail::max_arity(typename Api::routes{}) + 1>> {
public:
  /** @brief The contract it calls. */
  using api_type = Api;

  /** @brief The arguments of a call to `Route`, as a tuple. */
  template <class Endpoint>
  using arguments = detail::arguments_of<Api, Endpoint>;

  explicit client(Transport transport) : transport_(std::make_shared<Transport const>(std::move(transport))) {}

  /**
   * @brief A client of the server at `base_url`, through a transport built from it: with the default one, a connection
   * over the network of the executor that calls (see quoll::http::client, quoll::runtime), or the browser's fetch.
   *
   * A trailing `/` is dropped, so that every transport reads the same URL.
   *
   * @code
   * quoll::rest::client<bank::v1> bank{"http://127.0.0.1:8080"};
   * @endcode
   */
  explicit client(std::string base_url)
    requires std::constructible_from<Transport, std::string>
      : transport_(std::make_shared<Transport const>(without_trailing_slash(std::move(base_url)))) {}

  /**
   * @brief A client of the server at `base_url`, as above, with the options of its transport, if it has any.
   *
   * @code
   * quoll::rest::client<bank::v1> bank{"http://127.0.0.1:8080", {.response_timeout = 5s}};
   * @endcode
   */
  template <class T = Transport>
  client(std::string base_url, typename T::options_type options)
    requires std::constructible_from<T, std::string, typename T::options_type>
      : transport_(std::make_shared<Transport const>(without_trailing_slash(std::move(base_url)), std::move(options))) {
  }

  /** @brief The credential sent, from now on, to the routes that require `Scheme`. */
  template <class Scheme>
  void credentials(std::string value) {
    static_assert(
        detail::uses_scheme<Scheme>(typename Api::routes{}), "quoll::rest::client: no route of the api requires "
                                                             "this scheme"
    );

    credentials_.insert_or_assign(std::string{detail::scheme_traits<Scheme>::key.view()}, std::move(value));
  }

  /**
   * @brief Wraps every request the client sends from now on, inside the interceptors added before: `wrapping` gets the
   * request and what sends it on (the next interceptor, then the transport), and returns the response.
   *
   * A copy of the client shares the interceptors it had, and one added after to either is its own. A call keeps what it
   * sends through until its task ends.
   *
   * @code
   * api.use(quoll::rest::retry{.attempts = 3});
   * api.use(refresh_on_401);   // an interceptor of the program's: see docs/rest.md
   * @endcode
   *
   * @throws std::invalid_argument for an empty interceptor.
   */
  void use(interceptor wrapping) {
    if (!wrapping) {
      throw std::invalid_argument{"quoll::rest::client::use: an empty interceptor"};
    }

    auto grown =
        interception_
            ? std::make_shared<detail::interception>(*interception_)
            : std::make_shared<detail::interception>(detail::interception{
                  {}, [transport = transport_](http::request message) { return transport->send(std::move(message)); }
              });

    grown->interceptors.push_back(std::make_shared<interceptor const>(std::move(wrapping)));
    interception_ = std::move(grown);
  }

  /** @brief The transport the client sends its requests with. */
  [[nodiscard]] Transport const &transport() const noexcept QUOLL_LIFETIMEBOUND { return *transport_; }

private:
  template <class, class, class>
  friend struct detail::client_calls;

  // Through the interceptors, if any: the transport alone otherwise, as before any was added.
  [[nodiscard]] async::task<http::response> send(http::request message) const {
    if (!interception_) {
      return transport_->send(std::move(message));
    }

    return detail::send_through(interception_, std::move(message));
  }

  template <class Endpoint>
  [[nodiscard]] async::task<typename detail::call_value<Endpoint>::type> call_with(arguments<Endpoint> args) const {
    static_assert(!detail::has_events<Endpoint>, "quoll::rest::client::call: the route answers events<>: subscribe");

    using route = typename Api::template route<Endpoint>;

    return detail::receive_value<route>(send(detail::build_request<route>(args, credentials_)));
  }

  template <class Endpoint>
  [[nodiscard]] async::task<detail::call_result<Endpoint>> try_call_with(arguments<Endpoint> args) const {
    static_assert(
        !detail::has_events<Endpoint>, "quoll::rest::client::try_call: the route answers events<>: subscribe"
    );

    using route = typename Api::template route<Endpoint>;

    return detail::receive<route>(send(detail::build_request<route>(args, credentials_)));
  }

  // `auto`, so that a route without events<> meets the static_assert, not a failure to name what it returns.
  template <class Endpoint>
  [[nodiscard]] auto subscribe_with(arguments<Endpoint> args) const {
    static_assert(
        detail::has_events<Endpoint>, "quoll::rest::client::subscribe: the route answers no events<>: call it"
    );
    static_assert(
        http::StreamingTransport<Transport>,
        "quoll::rest::client::subscribe: the transport gives no response as its head comes (no stream())"
    );

    if constexpr (detail::has_events<Endpoint> && http::StreamingTransport<Transport>) {
      // Interceptors take whole responses: rather than go out without what they add, a token say, it refuses.
      if (interception_) {
        throw std::logic_error{"quoll::rest::client::subscribe: a client with interceptors, which take whole "
                               "responses: subscribe through one without them"};
      }

      using route = typename Api::template route<Endpoint>;

      auto request = detail::build_request<route>(args, credentials_);

      // One the contract declares stays, a third-party API's own media type say, which the answer may then carry.
      if (!request.fields.contains("Accept")) {
        request.fields.set("Accept", "text/event-stream");
      }

      auto accepted = std::string{request.fields.find("Accept").value_or("")};

      return detail::open_events<route>(transport_, std::move(request), std::move(accepted));
    }
  }

  [[nodiscard]] static std::string without_trailing_slash(std::string url) {
    // One only: `http://example.com//` stays what http::client refuses.
    if (url.ends_with('/') && !url.ends_with("//")) {
      url.pop_back();
    }

    return url;
  }

  // Shared by its copies and its interceptors: sent with through const functions only, as an http::HttpTransport is.
  std::shared_ptr<Transport const> transport_;
  detail::credential_map credentials_;
  // Null until use(): a client with no interceptor sends straight through its transport.
  std::shared_ptr<detail::interception const> interception_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_CLIENT_HPP
