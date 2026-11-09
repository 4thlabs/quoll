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

#ifndef QUOLL_HTTP_WEBSOCKET_HANDSHAKE_HPP
#define QUOLL_HTTP_WEBSOCKET_HANDSHAKE_HPP

#include <quoll/http/request.hpp>

#include <string>
#include <string_view>

namespace quoll::http {

/**
 * @brief Whether `message` asks for a WebSocket (RFC 6455, 4.2.1): a GET in HTTP/1.1 with a `Host`, `Upgrade:
 * websocket`, `Connection: Upgrade`, `Sec-WebSocket-Version: 13`, and one `Sec-WebSocket-Key` of 16 bytes in base64.
 */
[[nodiscard]] bool is_websocket_upgrade(request const &message) noexcept;

/**
 * @brief The `Sec-WebSocket-Accept` that answers `key`, spaces around it aside: the base64 of the SHA-1 of the key and
 * the GUID of RFC 6455.
 */
[[nodiscard]] std::string websocket_accept(std::string_view key);

} // namespace quoll::http

#endif // QUOLL_HTTP_WEBSOCKET_HANDSHAKE_HPP
