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
#include <quoll/io/network.hpp>
#include <quoll/server/serve.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace quoll::server {

/** @brief Where listen() listens, and how it serves. */
struct listen_options {
  /** @brief The IP address to listen on: loopback by default, `0.0.0.0` or `::` for every interface. */
  std::string address = "127.0.0.1";
  serve_options serve;
};

/**
 * @brief Listens on `port` and serves `router` there, until a stop request: the server of a program.
 *
 * Listens through the network of the executor the task runs on (io::listen): run it with sync_wait, which stops it on
 * Ctrl-C. Throws std::system_error when the address cannot be bound, and fails like serve() otherwise. `router`
 * outlives the task and the connections it serves.
 *
 * @code
 * quoll::sync_wait(quoll::server::listen(router, 8080));
 * @endcode
 */
[[nodiscard]] inline async::task<> listen(http::router const &router, std::uint16_t port, listen_options options = {}) {
  auto listener = co_await io::listen(port, std::move(options.address));

  co_await serve(listener, router, std::move(options.serve));
}

} // namespace quoll::server

#endif // QUOLL_SERVER_LISTEN_HPP
