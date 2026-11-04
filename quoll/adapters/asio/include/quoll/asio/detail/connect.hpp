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

#ifndef QUOLL_ASIO_DETAIL_CONNECT_HPP
#define QUOLL_ASIO_DETAIL_CONNECT_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/asio/tcp_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// async_resolve ignores cancellation slots, and a lookup in progress (getaddrinfo, on asio's resolver thread) cannot be
// interrupted. So a cancellation completes the operation at once, with operation_aborted, and the lookup's own
// completion is dropped when it comes. Both run on the resolver's executor: the first one completes the operation.
struct resolve_initiation {
  ::asio::ip::tcp::resolver *resolver;
  std::string const *host;
  std::string const *service;

  template <class Handler>
  void operator()(Handler handler) const {
    auto pending = std::make_shared<std::optional<Handler>>(std::move(handler));
    auto slot = ::asio::get_associated_cancellation_slot(**pending);

    if (slot.is_connected()) {
      slot.assign([pending](::asio::cancellation_type) {
        complete(*pending, ::asio::error::operation_aborted, ::asio::ip::tcp::resolver::results_type{});
      });
    }

    try {
      resolver->async_resolve(*host, *service,
                              [pending](std::error_code error, ::asio::ip::tcp::resolver::results_type endpoints) {
                                complete(*pending, error, std::move(endpoints));
                              });
    } catch (...) {
      // Not started: nothing left to cancel.
      if (slot.is_connected()) {
        slot.clear();
      }

      throw;
    }
  }

  template <class Handler>
  static void complete(std::optional<Handler> &pending, std::error_code error,
                       ::asio::ip::tcp::resolver::results_type endpoints) {
    if (!pending) {
      return;
    }

    auto handler = std::move(*pending);

    pending.reset();
    std::move(handler)(error, std::move(endpoints));
  }
};

inline async::task<::asio::ip::tcp::socket> connect_socket(::asio::any_io_executor executor, std::string host,
                                                           std::uint16_t port) {
  auto const cancel = std::make_shared<cancellation>(executor);
  ::asio::ip::tcp::resolver resolver{executor};
  auto const service = std::to_string(port);

  auto [resolve_error, endpoints] = co_await make_operation<::asio::ip::tcp::resolver::results_type>(
      cancel, resolve_initiation{&resolver, &host, &service});

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

  co_return socket;
}

inline async::task<io::any_stream> connect(::asio::any_io_executor executor, std::string host, std::uint16_t port) {
  co_return io::any_stream{tcp_stream{co_await connect_socket(std::move(executor), std::move(host), port)}};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_CONNECT_HPP
