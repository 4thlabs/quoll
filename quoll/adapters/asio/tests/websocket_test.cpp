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

#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/client/connect_websocket.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/run_task.hpp>
#include <quoll/test/websocket_echo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>
#include <utility>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using quoll::async::task;

// A client opening a WebSocket by its URL, through the runtime's network, to an http::server on it: what it sends
// comes back, then both ends close.
task<std::string> echo_over_tcp() {
  http::router router;

  router.get("/echo",
             [](http::request request) { return http::accept_websocket(request, quoll::test::websocket_echo); });

  auto listener = co_await io::listen(0);
  auto const url = "ws://127.0.0.1:" + std::to_string(listener.local_port().value()) + "/echo";
  http::server const server{router};
  std::stop_source stop;

  auto const serving = [&]() -> task<> {
    co_await quoll::async::detail::stop_scope<void>{server.serve(std::move(listener)), stop.get_token()};
  };
  auto const talking = [&]() -> task<std::string> {
    auto [socket, handshake] = co_await http::connect_websocket(url);

    co_await socket.send_text("over TCP");

    auto const answer = co_await socket.receive();

    co_await socket.close();

    auto const closed = co_await socket.receive();

    stop.request_stop();
    co_return closed.opcode == http::websocket_opcode::close ? answer.data : std::string{};
  };

  auto [served, answer] = co_await quoll::async::when_all(serving(), talking());

  co_return answer;
}

} // namespace

TEST_CASE("asio: a WebSocket client and server talk over TCP, on a runtime", "[asio][websocket]") {
  REQUIRE(quoll::test::run_task(echo_over_tcp()) == "over TCP");
}
