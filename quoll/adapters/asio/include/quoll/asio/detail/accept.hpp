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

#ifndef QUOLL_ASIO_DETAIL_ACCEPT_HPP
#define QUOLL_ASIO_DETAIL_ACCEPT_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/asio/tcp_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>

#include <asio/ip/tcp.hpp>

#include <memory>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// What an acceptor keeps at a stable address: the listening socket and the cancellation of its accepts.
struct acceptor_state {
  explicit acceptor_state(::asio::ip::tcp::acceptor &&listening)
      : acceptor(std::move(listening)), cancel(std::make_shared<cancellation>(acceptor.get_executor())) {}

  ::asio::ip::tcp::acceptor acceptor;
  std::shared_ptr<cancellation> cancel;
};

// What an HTTP response in several writes would otherwise wait for: the last of them, too small to fill a packet, stays
// until the peer acknowledges the one before, which it delays, by 40 ms on Linux. A socket refusing this works all the
// same.
inline void disable_nagle(::asio::ip::tcp::socket &socket) noexcept {
  std::error_code ignored;

  socket.set_option(::asio::ip::tcp::no_delay{true}, ignored);
}

inline async::task<::asio::ip::tcp::socket> accept_socket(acceptor_state &state) {
  // Accepted into a socket of its own: the operation's values are default constructible, a socket is not.
  ::asio::ip::tcp::socket socket{state.acceptor.get_executor()};

  auto const [error] = co_await make_operation<>(
      state.cancel, [&](auto handler) { state.acceptor.async_accept(socket, std::move(handler)); });

  if (error) {
    throw std::system_error{to_io_error(error), "quoll::asio::acceptor: cannot accept"};
  }

  disable_nagle(socket);
  co_return socket;
}

inline async::task<io::any_stream> accept(acceptor_state &state) {
  co_return io::any_stream{tcp_stream{co_await accept_socket(state)}};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_ACCEPT_HPP
