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

#ifndef QUOLL_ASIO_DETAIL_CONNECT_LOCAL_HPP
#define QUOLL_ASIO_DETAIL_CONNECT_LOCAL_HPP

#include <asio/detail/config.hpp>

#if defined(ASIO_HAS_LOCAL_SOCKETS)

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/io_error.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/asio/unix_stream.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_stream.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/local/stream_protocol.hpp>

#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// A connection to the Unix domain socket at `path`, cancelled as a TCP one is, on a stop.
inline async::task<io::any_stream> connect_local(::asio::any_io_executor executor, std::string path) {
  auto const cancel = std::make_shared<cancellation>(executor);
  ::asio::local::stream_protocol::endpoint const endpoint{path};
  ::asio::local::stream_protocol::socket socket{executor};

  auto const [error] =
      co_await make_operation<>(cancel, [&](auto handler) { socket.async_connect(endpoint, std::move(handler)); });

  if (error) {
    throw std::system_error{to_io_error(error), "quoll::asio: cannot connect to " + path};
  }

  co_return io::any_stream{unix_stream{std::move(socket)}};
}

} // namespace quoll::asio::detail

#endif // ASIO_HAS_LOCAL_SOCKETS

#endif // QUOLL_ASIO_DETAIL_CONNECT_LOCAL_HPP
