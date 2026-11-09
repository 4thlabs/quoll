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

#ifndef QUOLL_CLIENT_WEBSOCKET_REFUSED_HPP
#define QUOLL_CLIENT_WEBSOCKET_REFUSED_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/response.hpp>

#include <memory>
#include <stdexcept>

namespace quoll::http {

/**
 * @brief Thrown by http::connect_websocket for a server that does not switch to a WebSocket: any other answer, or a 101
 * that is not what the handshake asked for. Its response says which.
 *
 * Thread safety: its copies share one response, never changed, which they may read from any thread.
 */
class websocket_refused : public std::runtime_error {
public:
  /** @brief Refused with `answer`, the server's. */
  explicit websocket_refused(http::response answer);

  // Copies and no moves: an exception does not throw on its way to a handler, and one moved from stays whole.
  websocket_refused(websocket_refused const &) noexcept = default;
  websocket_refused &operator=(websocket_refused const &) noexcept = default;
  ~websocket_refused() override = default;

  /** @brief The server's answer: its head, the body of a refusal left unread. */
  [[nodiscard]] http::response const &response() const noexcept QUOLL_LIFETIMEBOUND { return *response_; }

private:
  std::shared_ptr<http::response const> response_;
};

} // namespace quoll::http

#endif // QUOLL_CLIENT_WEBSOCKET_REFUSED_HPP
