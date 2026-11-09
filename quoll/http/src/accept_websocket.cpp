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

#include <quoll/http/accept_websocket.hpp>

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/websocket_handshake.hpp>
#include <quoll/http/websocket_parser.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::http {

namespace {

// A handshake but for its version, which is not the one this speaks.
[[nodiscard]] bool asks_another_version(request const &message) {
  auto const version = message.fields.find("Sec-WebSocket-Version");

  return message.method == verb::get && version && detail::trim(*version) != "13" &&
         detail::field_names(message.fields, "Upgrade", "websocket");
}

// All it runs on held by its frame: the handler, a copy of the response's, outlives the task it gives.
async::task<> run_websocket(std::function<async::task<>(websocket)> handler, io::any_stream stream,
                            std::string received, websocket_options options) {
  co_await handler(websocket{std::move(stream), websocket_role::server, std::move(received), options});
}

} // namespace

response accept_websocket(request const &message, std::function<async::task<>(websocket)> handler,
                          websocket_options options) {
  if (!handler) {
    throw std::invalid_argument{"quoll::http::accept_websocket: an empty handler"};
  }

  if (!is_websocket_upgrade(message)) {
    if (!asks_another_version(message)) {
      return detail::status_response(status::bad_request);
    }

    auto refused = detail::status_response(status::upgrade_required);

    refused.fields.set("Sec-WebSocket-Version", "13");
    return refused;
  }

  response accepted;

  accepted.status = status::switching_protocols;
  accepted.fields.set("Upgrade", "websocket");
  accepted.fields.set("Connection", "Upgrade");
  accepted.fields.set("Sec-WebSocket-Accept", websocket_accept(message.fields.find("Sec-WebSocket-Key").value_or("")));
  accepted.upgrade = [handler = std::move(handler), options](io::any_stream stream, std::string received) {
    return run_websocket(handler, std::move(stream), std::move(received), options);
  };

  return accepted;
}

} // namespace quoll::http
