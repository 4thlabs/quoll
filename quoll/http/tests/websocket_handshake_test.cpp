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

#include <quoll/http/request.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/http/websocket_handshake.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

namespace http = quoll::http;

// The upgrade request of RFC 6455's example.
[[nodiscard]] http::request upgrade() {
  http::request message;

  message.method = http::verb::get;
  message.target = "/chat";
  message.fields.set("Host", "server.example.com");
  message.fields.set("Upgrade", "websocket");
  message.fields.set("Connection", "Upgrade");
  message.fields.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
  message.fields.set("Sec-WebSocket-Version", "13");
  return message;
}

} // namespace

TEST_CASE("websocket_accept: the example of RFC 6455", "[http][websocket]") {
  REQUIRE(http::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("is_websocket_upgrade: what asks for one, and what does not", "[http][websocket]") {
  REQUIRE(http::is_websocket_upgrade(upgrade()));

  auto tokens = upgrade();

  tokens.fields.set("Upgrade", "h2c, WebSocket");
  tokens.fields.set("Connection", "keep-alive, upgrade");
  REQUIRE(http::is_websocket_upgrade(tokens));

  auto const without = [](auto change) {
    auto message = upgrade();

    change(message);
    return http::is_websocket_upgrade(message);
  };

  REQUIRE_FALSE(without([](http::request &message) { message.method = http::verb::post; }));
  REQUIRE_FALSE(without([](http::request &message) { message.version = http::version::http_1_0; }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.erase("Upgrade"); }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.set("Connection", "keep-alive"); }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.set("Sec-WebSocket-Version", "8"); }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.erase("Sec-WebSocket-Key"); }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.set("Sec-WebSocket-Key", "short"); }));
  REQUIRE_FALSE(without([](http::request &message) {
    message.fields.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=!");
  }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.erase("Host"); }));
  REQUIRE_FALSE(without([](http::request &message) {
    message.fields.add("Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAA==");
  }));
  REQUIRE_FALSE(without([](http::request &message) { message.fields.add("Sec-WebSocket-Version", "8"); }));
  // Padding bits set in the last digit: no key of 16 bytes.
  REQUIRE_FALSE(without([](http::request &message) {
    message.fields.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZR==");
  }));
  // A digit that is no base64 among the first 22, with the right end.
  REQUIRE_FALSE(without([](http::request &message) {
    message.fields.set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25j!Q==");
  }));
}

TEST_CASE("websocket handshake: spaces around the key and the version are no part of them", "[http][websocket]") {
  auto spaced = upgrade();

  spaced.fields.set("Sec-WebSocket-Key", " dGhlIHNhbXBsZSBub25jZQ== ");
  spaced.fields.set("Sec-WebSocket-Version", " 13 ");

  REQUIRE(http::is_websocket_upgrade(spaced));
  REQUIRE(http::websocket_accept(" dGhlIHNhbXBsZSBub25jZQ== ") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
