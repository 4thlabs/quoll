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

#ifndef QUOLL_IO_LISTEN_HPP
#define QUOLL_IO_LISTEN_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/detail/require_service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/network_ref.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::io {

/** @brief Where io::listen() and quoll::server::listen() listen by default: the loopback interface only. */
inline constexpr std::string_view default_listen_address = "127.0.0.1";

/**
 * @brief Listens on `port` (0 for any free port) and `address` (an IP address; loopback by default, `0.0.0.0` for
 * every interface), through the network of the executor the task runs on (as quoll::runtime provides it).
 *
 * Throws std::system_error when the address cannot be bound, std::logic_error if the executor offers no network.
 */
[[nodiscard]] inline async::task<any_listener> listen(std::uint16_t port,
                                                      std::string address = std::string{default_listen_address}) {
  auto const network =
      async::detail::require_service<network_ref>(co_await async::current_executor(), "quoll::io::listen", "network");

  co_return network.listen(std::move(address), port);
}

} // namespace quoll::io

#endif // QUOLL_IO_LISTEN_HPP
