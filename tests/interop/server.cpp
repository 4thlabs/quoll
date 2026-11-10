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

// The Quoll end third-party clients talk to (tests/interop/run.sh): HTTP, server-sent events and WebSocket, on
// 0.0.0.0 at the port given, so that the other containers of the run reach it.

#include <quoll/async/task.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/event_writer.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_writer.hpp>
#include <quoll/http/router.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/websocket_echo.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;

http::router make_router() {
  http::router router;

  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  router.post("/echo", [](http::request request) {
    http::response response;

    response.body = std::move(request.body);
    return response;
  });
  // A body in pieces of its own, with no length: chunked.
  router.get("/pieces/{count}", [](int count) {
    http::response response;

    response.body_writer = [count](http::body_stream out) -> async::task<> {
      for (auto index = 0; index < count; ++index) {
        co_await out.write("piece " + std::to_string(index) + "\n");
      }
    };
    return response;
  });
  router.get("/events/{count}", [](int count, http::response_writer out) -> async::task<> {
    http::event_writer events{std::move(out)};

    co_await events.open();

    for (auto index = 0; index < count; ++index) {
      co_await events.send({.type = "count", .data = "event " + std::to_string(index), .id = std::to_string(index)});
    }
  });
  // What closes its connection once answered.
  router.get("/close", [](http::request) {
    http::response response;

    response.fields.set("Connection", "close");
    response.body = "bye";
    return response;
  });
  router.get("/ws", [](http::request request) {
    // Autobahn sends messages of up to 16 MiB.
    // Every message back as it came, text or binary, as Autobahn's fuzzing client asks of a server.
    return http::accept_websocket(request, quoll::test::websocket_echo,
                                  {.max_message_size = std::size_t{64} * 1024 * 1024});
  });
  return router;
}

async::task<> serve(std::uint16_t port) {
  http::server const server{make_router()};

  std::cout << "quoll interop server on 0.0.0.0:" << port << std::endl;
  co_await server.listen(port, {.address = "0.0.0.0"});
}

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
  quoll::runtime runtime;

  return runtime.run(serve(port));
}
