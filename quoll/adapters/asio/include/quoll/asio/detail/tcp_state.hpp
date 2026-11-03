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

#ifndef QUOLL_ASIO_DETAIL_TCP_STATE_HPP
#define QUOLL_ASIO_DETAIL_TCP_STATE_HPP

#include <quoll/asio/detail/cancellation.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace quoll::asio::detail {

// The cancellation of each direction, allocated together.
struct tcp_cancellations {
  explicit tcp_cancellations(::asio::any_io_executor const &executor) noexcept : read(executor), write(executor) {}

  cancellation read;
  cancellation write;
};

// What a tcp_stream keeps at a stable address: the socket, and the cancellation of each direction.
struct tcp_state {
  explicit tcp_state(::asio::ip::tcp::socket &&connected)
      : socket(std::move(connected)), cancellations(std::make_shared<tcp_cancellations>(socket.get_executor())),
        read(cancellations, &cancellations->read), write(cancellations, &cancellations->write) {}

  ::asio::ip::tcp::socket socket;
  std::shared_ptr<tcp_cancellations> cancellations;
  // Share the ownership of `cancellations`.
  std::shared_ptr<cancellation> read;
  std::shared_ptr<cancellation> write;
};

struct read_some_initiation {
  ::asio::ip::tcp::socket *socket;
  std::span<std::byte> buffer;

  template <class Handler>
  void operator()(Handler &&handler) const {
    socket->async_read_some(::asio::buffer(buffer.data(), buffer.size()), std::forward<Handler>(handler));
  }
};

struct write_some_initiation {
  ::asio::ip::tcp::socket *socket;
  std::span<std::byte const> data;

  template <class Handler>
  void operator()(Handler &&handler) const {
    socket->async_write_some(::asio::buffer(data.data(), data.size()), std::forward<Handler>(handler));
  }
};

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_TCP_STATE_HPP
