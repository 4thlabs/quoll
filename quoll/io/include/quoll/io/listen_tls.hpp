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

#ifndef QUOLL_IO_LISTEN_TLS_HPP
#define QUOLL_IO_LISTEN_TLS_HPP

#include <quoll/async/current_executor.hpp>
#include <quoll/async/detail/require_service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/detail/tls_service.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/tls_network_ref.hpp>
#include <quoll/io/tls_options.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::io {

/**
 * @brief Listens for encrypted connections on `port` (0 for any free port) and `address` (an IP address; loopback by
 * default), through the TLS network of the executor the task runs on (as quoll::runtime provides it when built with a
 * TLS library).
 *
 * Throws std::invalid_argument without both a certificate and its key, std::system_error when they cannot be loaded or
 * the address cannot be bound, std::logic_error if the executor offers no TLS network.
 */
[[nodiscard]] inline async::task<any_listener> listen_tls(std::uint16_t port, tls_server_options options,
                                                          std::string address = std::string{default_listen_address}) {
  if (options.certificate_file.empty() || options.private_key_file.empty()) {
    throw std::invalid_argument{"quoll::io::listen_tls: both a certificate and its private key are needed"};
  }

  auto const network = async::detail::require_service<tls_network_ref>(
      co_await async::current_executor(), "quoll::io::listen_tls", detail::tls_service_name);

  co_return network.listen_tls(std::move(address), port, options);
}

} // namespace quoll::io

#endif // QUOLL_IO_LISTEN_TLS_HPP
