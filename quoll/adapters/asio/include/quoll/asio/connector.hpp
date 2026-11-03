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

#ifndef QUOLL_ASIO_CONNECTOR_HPP
#define QUOLL_ASIO_CONNECTOR_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/asio/tcp_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::asio {

namespace detail {

inline async::task<io::any_stream> connect(::asio::any_io_executor executor, std::string host, std::uint16_t port) {
  auto const cancel = std::make_shared<cancellation>(executor);
  ::asio::ip::tcp::resolver resolver{executor};
  auto const service = std::to_string(port);

  auto [resolve_error, endpoints] =
      co_await make_operation<::asio::ip::tcp::resolver::results_type>(cancel, [&](auto handler) {
        // async_resolve ignores the cancellation slot: a cancellation cancels the resolver instead.
        auto slot = ::asio::get_associated_cancellation_slot(handler);

        if (slot.is_connected()) {
          slot.assign([&resolver](::asio::cancellation_type) { resolver.cancel(); });
        }

        resolver.async_resolve(host, service, std::move(handler));
      });

  if (resolve_error) {
    throw std::system_error{to_io_error(resolve_error), "quoll::asio::connector: cannot resolve " + host};
  }

  ::asio::ip::tcp::socket socket{executor};

  auto const [connect_error, endpoint] = co_await make_operation<::asio::ip::tcp::endpoint>(
      cancel, [&](auto handler) { ::asio::async_connect(socket, endpoints, std::move(handler)); });

  if (connect_error) {
    throw std::system_error{to_io_error(connect_error),
                            "quoll::asio::connector: cannot connect to " + host + ":" + service};
  }

  co_return io::any_stream{tcp_stream{std::move(socket)}};
}

} // namespace detail

/**
 * @brief Connector opening TCP connections with asio: resolves the host, then tries its addresses in turn.
 *
 * The connection is a tcp_stream on `executor`, handed over as an io::any_stream. Resolution and connection failures
 * throw std::system_error from the task, with asio's error code (`asio::error::connection_refused`...). A stop
 * request, from any thread, abandons the attempt, with `std::errc::operation_canceled`. `executor` runs one handler
 * at a time (an io_context run by one thread, or a strand).
 *
 * The task copies what it needs: the connector does not have to outlive it.
 */
class connector {
public:
  explicit connector(::asio::any_io_executor executor) noexcept : executor_(std::move(executor)) {}

  /** @brief Connects to `host` (a name or an address) on `port`. */
  [[nodiscard]] async::task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return detail::connect(executor_, std::move(host), port);
  }

private:
  ::asio::any_io_executor executor_;
};

static_assert(io::Connector<connector>);

} // namespace quoll::asio

#endif // QUOLL_ASIO_CONNECTOR_HPP
