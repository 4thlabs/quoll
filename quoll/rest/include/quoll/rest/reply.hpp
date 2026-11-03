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

#ifndef QUOLL_REST_REPLY_HPP
#define QUOLL_REST_REPLY_HPP

#include <quoll/http/status.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/detail/response_value.hpp>
#include <quoll/rest/detail/type_list.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace quoll::rest {

/** @brief A value with the status it is sent with; built by respond(). */
template <http::status Status, class T>
struct status_value {
  T value;
};

template <http::status Status>
struct status_value<Status, void> {};

/** @brief `value`, to be sent with `Status`: picks a declared response when several have the type of `value`. */
template <http::status Status, class T>
[[nodiscard]] status_value<Status, std::decay_t<T>> respond(T &&value) {
  return {std::forward<T>(value)};
}

/** @brief The declared response with status `Status` and no body. */
template <http::status Status>
[[nodiscard]] status_value<Status, void> respond() {
  return {};
}

/**
 * @brief One of the responses `Endpoint` declares with returns<>: what a service returns when it does not always
 * answer with the first one, and what a client's `try_call` hands back.
 *
 * Built from a value whose type only one response has (`reply<users::get>{user}`), or from respond<Status>(value)
 * otherwise. A status or a type the endpoint does not declare does not compile. Only the endpoint's own responses:
 * those inherited from with<> are answered by the server (credentials, errors), not by services.
 */
template <class Endpoint>
class reply {
public:
  using responses = detail::filter_t<detail::is_response_part, typename Endpoint::parts>;

  static_assert(responses::size > 0, "quoll::rest::reply: the endpoint declares no returns<>");

  // Not explicit, nor the next one: a service co_returns the value, or respond<Status>(value), as it is.
  template <class T>
    requires(detail::count_true(detail::with_type<std::decay_t<T>>(responses{})) == 1)
  reply(T &&value)
      : value_(std::in_place_index<detail::first_true(detail::with_type<std::decay_t<T>>(responses{}))>,
               std::forward<T>(value)) {}

  template <http::status Status, class T>
    requires(detail::first_true(detail::with_status<Status, T>(responses{})) < responses::size)
  reply(status_value<Status, T> value)
      : value_(make<detail::first_true(detail::with_status<Status, T>(responses{}))>(std::move(value))) {}

  /** @brief The status of the response held. */
  [[nodiscard]] http::status status() const noexcept {
    return std::visit([](auto const &held) { return std::decay_t<decltype(held)>::response::status; }, value_);
  }

  /** @brief Whether the response held has `Status`. */
  template <http::status Status>
  [[nodiscard]] bool holds() const noexcept {
    return status() == Status;
  }

  /**
   * @brief The body of the response with `Status`.
   * @throws std::bad_variant_access when another response is held.
   */
  template <http::status Status>
  [[nodiscard]] auto &get() {
    return std::get<status_index<Status>()>(value_).value;
  }

  template <http::status Status>
  [[nodiscard]] auto const &get() const {
    return std::get<status_index<Status>()>(value_).value;
  }

  /** @brief Calls `visitor` with the detail::response_value held; for the server and the client. */
  template <class Visitor>
  decltype(auto) visit(Visitor &&visitor) const {
    return std::visit(std::forward<Visitor>(visitor), value_);
  }

private:
  using storage = typename detail::response_variant<responses>::type;

  template <std::size_t Index, http::status Status, class T>
  [[nodiscard]] static storage make(status_value<Status, T> &&value) {
    if constexpr (std::is_void_v<T>) {
      return storage{std::in_place_index<Index>};
    } else {
      return storage{std::in_place_index<Index>, std::move(value.value)};
    }
  }

  template <http::status Status>
  [[nodiscard]] static consteval std::size_t status_index() {
    constexpr auto index = detail::first_true(detail::with_status_only<Status>(responses{}));

    static_assert(index < responses::size, "quoll::rest::reply: the endpoint declares no response with this status");
    return index;
  }

  storage value_;
};

} // namespace quoll::rest

#endif // QUOLL_REST_REPLY_HPP
