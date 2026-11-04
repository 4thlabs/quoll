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

#ifndef QUOLL_SERVER_LISTEN_HPP
#define QUOLL_SERVER_LISTEN_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/listen_tls.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/server/serve.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace quoll::server {

/** @brief Where listen() listens, and how it serves. */
struct listen_options {
  /** @brief The IP address to listen on: loopback by default, `0.0.0.0` or `::` for every interface. */
  std::string address = std::string{io::default_listen_address};
  /** @brief A certificate and its key make it an HTTPS server; without them, it speaks plain HTTP. */
  io::tls_server_options tls{};
  serve_options serve{};
};

/**
 * @brief Listens on `port` and serves `router` there, until a stop request: the server of a program.
 *
 * Listens through the network of the executor the task runs on (io::listen), a runtime's: spawned with the runtime's
 * stop token, it stops on Ctrl-C. Throws std::system_error when the address cannot be bound or the certificate loaded,
 * and fails like serve() otherwise. `router` outlives the task, which completes once its connections have ended.
 *
 * @code
 * quoll::async::task<> serve_notes(std::uint16_t port) {
 *   quoll::http::router router;
 *
 *   quoll::rest::mount<notes::api>(router, notes_service{});
 *   co_await quoll::server::listen(router, port);
 * }
 * @endcode
 *
 * Over TLS:
 *
 * @code
 * co_await quoll::server::listen(router, 8443, {.tls = {.certificate_file = "cert.pem", .private_key_file =
 * "key.pem"}});
 * @endcode
 */
[[nodiscard]] inline async::task<> listen(http::router const &router, std::uint16_t port, listen_options options = {}) {
  auto listener = options.tls.enabled()
                      ? co_await io::listen_tls(port, std::move(options.tls), std::move(options.address))
                      : co_await io::listen(port, std::move(options.address));

  co_await serve(listener, router, std::move(options.serve));
}

} // namespace quoll::server

#endif // QUOLL_SERVER_LISTEN_HPP
