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

#ifndef QUOLL_IO_TLS_NETWORK_HPP
#define QUOLL_IO_TLS_NETWORK_HPP

#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/tls_options.hpp>

#include <concepts>
#include <cstdint>
#include <string>

namespace quoll::io {

/**
 * @brief A network speaking TLS: encrypted connections out (`connect_tls`) and in (`listen_tls`).
 *
 * Typically the executor of an I/O loop, offering itself as such when it was built with a TLS library (see
 * tls_network_ref and async::service_list). `connect_tls` completes once the handshake has checked the server, or
 * throws std::system_error. `listen_tls` loads the certificate and binds, or throws std::system_error; the handshake of
 * a connection it accepts takes place on its first read, so that a slow peer holds nothing but its own connection.
 */
template <class N>
concept TlsNetwork = requires(N const &network, std::string text, std::uint16_t port, tls_client_options const &client,
                              tls_server_options const &server) {
  { network.connect_tls(text, port, client) } -> std::same_as<async::task<any_stream>>;
  { network.listen_tls(text, port, server) } -> std::same_as<any_listener>;
};

} // namespace quoll::io

#endif // QUOLL_IO_TLS_NETWORK_HPP
