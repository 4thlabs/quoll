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

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>

#include <array>
#include <cstddef>
#include <system_error>

namespace quoll::asio::detail {

// What a read on `socket` would do now, as a peek that does not wait tells, as curl checks a connection: bytes, the
// end of the stream or an error are ready. The socket is put in its non-blocking mode for that peek only, which asio's
// asynchronous operations do not depend on.
[[nodiscard]] inline io::read_readiness poll_socket(::asio::ip::tcp::socket &socket) noexcept {
  std::error_code error;
  auto const was_non_blocking = socket.non_blocking();

  static_cast<void>(socket.non_blocking(true, error));

  if (error) {
    return io::read_readiness::unknown;
  }

  std::array<std::byte, 1> peeked{};

  static_cast<void>(socket.receive(::asio::buffer(peeked), ::asio::socket_base::message_peek, error));

  std::error_code restored;

  static_cast<void>(socket.non_blocking(was_non_blocking, restored));

  if (error == ::asio::error::would_block || error == ::asio::error::try_again) {
    return io::read_readiness::would_wait;
  }

  return io::read_readiness::ready;
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_POLL_SOCKET_HPP
