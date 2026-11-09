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

#ifndef QUOLL_CLIENT_DETAIL_CONNECT_WEBSOCKET_HPP
#define QUOLL_CLIENT_DETAIL_CONNECT_WEBSOCKET_HPP

#include <quoll/async/task.hpp>
#include <quoll/client/connected_websocket.hpp>
#include <quoll/client/detail/connector_handle.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/client/websocket_connect_options.hpp>
#include <quoll/http/fields.hpp>

#include <string>
#include <vector>

namespace quoll::http::detail {

// A client's handshake, ready to send: its request on the wire, its key, and the subprotocols it offers.
struct websocket_handshake {
  std::string wire;
  std::string key;
  std::vector<std::string> offered;
};

// The handshake to `where` with `fields`, checked: std::invalid_argument for a field of the handshake's own, or a
// request that would not serialize.
[[nodiscard]] websocket_handshake prepare_websocket_handshake(websocket_target const &where, http::fields fields);

// The rest of connect_websocket(), through `connector` to `server`, compiled once.
[[nodiscard]] async::task<connected_websocket> connect_websocket_through(connector_handle connector, origin server,
                                                                         websocket_handshake handshake,
                                                                         websocket_connect_options options);

} // namespace quoll::http::detail

#endif // QUOLL_CLIENT_DETAIL_CONNECT_WEBSOCKET_HPP
