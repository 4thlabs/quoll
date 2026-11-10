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

#ifndef QUOLL_ASIO_DETAIL_POLL_SOCKET_HPP
#define QUOLL_ASIO_DETAIL_POLL_SOCKET_HPP

#include <quoll/io/pollable_stream.hpp>

#include <asio/ip/tcp.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>

#include <cerrno>
#endif

namespace quoll::asio::detail {

// What a read on `socket` would do now, as a poll that does not wait tells, as curl checks a connection: one call,
// which leaves the socket's mode and what it holds alone. Readable is bytes, the end of the stream or an error; a
// socket closed here, or a poll that fails, is nothing a request can count on either.
[[nodiscard]] inline io::read_readiness poll_socket(::asio::ip::tcp::socket &socket) noexcept {
  if (!socket.is_open()) {
    return io::read_readiness::ready;
  }

#if defined(_WIN32)
  ::WSAPOLLFD polled{socket.native_handle(), POLLRDNORM, 0};
  auto const found = ::WSAPoll(&polled, 1, 0);
#else
  ::pollfd polled{socket.native_handle(), POLLIN, 0};
  auto found = ::poll(&polled, 1, 0);

  // A signal before it looked: it looks again.
  while (found < 0 && errno == EINTR) {
    found = ::poll(&polled, 1, 0);
  }
#endif

  return found == 0 ? io::read_readiness::would_wait : io::read_readiness::ready;
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_POLL_SOCKET_HPP
