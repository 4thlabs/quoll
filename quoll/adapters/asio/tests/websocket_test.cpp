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

#include <quoll/asio/runtime.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/client/connect_websocket.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run_task.hpp>
#include <quoll/test/under_stop.hpp>
#include <quoll/test/websocket_echo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
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
    co_await quoll::test::under_stop(server.serve(std::move(listener)), stop.get_token());
  };
  auto const talking = [&]() -> task<std::string> {
    // The server stopped however this ends, a failure included, which would leave it serving otherwise.
    quoll::async::detail::stop_on_exit const stop_server{stop};
    auto [socket, handshake] = co_await http::connect_websocket(url);

    co_await socket.send_text("over TCP");

    auto const answer = co_await socket.receive();

    co_await socket.close();

    auto const closed = co_await socket.receive();

    co_return closed.opcode == http::websocket_opcode::close ? answer.data : std::string{};
  };

  auto [served, answer] = co_await quoll::async::when_all(serving(), talking());

  co_return answer;
}

// A client whose server answers none of its pings, over TCP on a runtime of several threads, sending beside its
// receive(): its keep-alive takes the server as gone, which the receive() and the sends throw. That the keep-alive's
// pings take turns with the receive() and the sends is for the TSan job to check.
task<std::error_code> unanswered_over_tcp() {
  http::router router;

  // A handler that never receives: the client's pings go unanswered. It ends in 5 s, which a receive() whose keep-alive
  // failed would wait for.
  router.get("/quiet", [](http::request request) {
    return http::accept_websocket(
        request, [](http::websocket) -> task<> { co_await quoll::async::sleep_for(std::chrono::seconds{5}); });
  });

  auto listener = co_await io::listen(0);
  auto const url = "ws://127.0.0.1:" + std::to_string(listener.local_port().value()) + "/quiet";
  http::server const server{router};
  std::stop_source stop;

  auto const serving = [&]() -> task<> {
    co_await quoll::test::under_stop(server.serve(std::move(listener)), stop.get_token());
  };
  auto const receiving = [&]() -> task<std::error_code> {
    quoll::async::detail::stop_on_exit const stop_server{stop};
    http::websocket_connect_options options;

    options.websocket.ping_interval = std::chrono::milliseconds{50};

    auto [socket, handshake] = co_await http::connect_websocket(url, std::move(options));

    // Sends beside the receive(), as its pings leave, until the keep-alive takes the server as gone.
    auto const sending = [&socket]() -> task<> {
      try {
        for (;;) {
          co_await socket.send_text("still there?");
          co_await quoll::async::sleep_for(std::chrono::milliseconds{10});
        }
      } catch (std::system_error const &) {
        // The verdict, which ends the sends too.
      }
    };
    auto const receiving_one = [&socket]() -> task<std::error_code> {
      try {
        static_cast<void>(co_await socket.receive());
      } catch (std::system_error const &gone) {
        co_return gone.code();
      }

      co_return std::error_code{};
    };

    auto [sent, code] = co_await quoll::async::when_all(sending(), receiving_one());

    co_return code;
  };

  auto [served, code] = co_await quoll::async::when_all(serving(), receiving());

  co_return code;
}

} // namespace

TEST_CASE("asio: a WebSocket client's keep-alive takes a quiet server as gone, on a runtime of several threads",
          "[asio][websocket]") {
  quoll::asio::runtime runtime{{.threads = 4}};
  std::optional<std::error_code> code;
  std::exception_ptr failure;

  quoll::async::co_spawn(runtime.get_executor(), unanswered_over_tcp(),
                         quoll::test::overloaded{[&](std::error_code ended) { code = ended; },
                                                 [&](std::exception_ptr error) { failure = std::move(error); }});
  runtime.run();

  if (failure) {
    std::rethrow_exception(failure);
  }

  REQUIRE(code == std::errc::timed_out);
}

TEST_CASE("asio: a WebSocket client and server talk over TCP, on a runtime", "[asio][websocket]") {
  REQUIRE(quoll::test::run_task(echo_over_tcp()) == "over TCP");
}
