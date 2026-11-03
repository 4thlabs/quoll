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

#ifndef QUOLL_ASIO_DETAIL_NETWORK_HPP
#define QUOLL_ASIO_DETAIL_NETWORK_HPP

#include <quoll/asio/acceptor.hpp>
#include <quoll/asio/detail/connect.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/network.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace quoll::asio::detail {

// The network of an asio executor: connections and listening sockets on its execution context. Points to the
// executor that offered it.
template <class AsioExecutor>
struct network {
  AsioExecutor const *executor;

  [[nodiscard]] async::task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return detail::connect(::asio::any_io_executor{*executor}, std::move(host), port);
  }

  [[nodiscard]] io::any_listener listen(std::string address, std::uint16_t port) const {
    ::asio::ip::tcp::endpoint const endpoint{::asio::ip::make_address(address), port};

    return io::any_listener{acceptor{::asio::ip::tcp::acceptor{*executor, endpoint}}};
  }
};

static_assert(io::Network<network<::asio::any_io_executor>>);

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_NETWORK_HPP
