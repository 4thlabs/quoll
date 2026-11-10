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

// The Quoll end that talks to third-party servers (tests/interop/run.sh), which serve the routes of server.cpp:
//
//   quoll_interop_client http <http://host:port>     /hello, /echo, /pieces/{n}, /close, kept connections, POSTs
//   quoll_interop_client sse <http://host:port>      /events/{n}
//   quoll_interop_client ws <ws://host:port/ws>      an echo, text and binary
//   quoll_interop_client autobahn <ws://host:port>   Autobahn's fuzzing server, as the client its cases test
//
// Each check prints "ok <name>" or "FAIL <name>: why"; the exit status is a failure if one failed.

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client/client.hpp>
#include <quoll/client/connect_websocket.hpp>
#include <quoll/client/websocket_connect_options.hpp>
#include <quoll/http/event_reader.hpp>
#include <quoll/http/is_event_stream.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/runtime.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;

int failures = 0;

// Runs `check`, which throws what went wrong, and says how it went.
async::task<> check(std::string name, std::function<async::task<>()> body) {
  try {
    co_await body();
    std::cout << "ok " << name << std::endl;
  } catch (std::exception const &failure) {
    ++failures;
    std::cout << "FAIL " << name << ": " << failure.what() << std::endl;
  }
}

void expect(bool holds, std::string const &what) {
  if (!holds) {
    throw std::runtime_error{what};
  }
}

std::string expected_pieces(int count) {
  std::string all;

  for (auto index = 0; index < count; ++index) {
    all += "piece " + std::to_string(index) + "\n";
  }

  return all;
}

async::task<> http_checks(std::string base) {
  http::client const client{base};

  co_await check("GET /hello", [&]() -> async::task<> {
    auto const response = co_await client.get("/hello");

    expect(response.status == http::status::ok, "status " + std::to_string(static_cast<int>(response.status)));
    expect(response.body == "hello", "body \"" + response.body + "\"");
  });
  co_await check("five GETs on a kept connection", [&]() -> async::task<> {
    for (auto turn = 0; turn < 5; ++turn) {
      expect((co_await client.get("/hello")).body == "hello", "GET " + std::to_string(turn));
    }
  });
  co_await check("three POSTs echoed", [&]() -> async::task<> {
    for (auto turn = 0; turn < 3; ++turn) {
      auto const body = "ping " + std::to_string(turn);
      auto const response = co_await client.post("/echo", body, "text/plain");

      expect(response.body == body, "POST " + std::to_string(turn) + " got \"" + response.body + "\"");
    }
  });
  co_await check("a POST of 1 MiB echoed", [&]() -> async::task<> {
    std::string const body(std::size_t{1024} * 1024, 'x');
    auto const response = co_await client.post("/echo", body, "application/octet-stream");

    expect(response.body.size() == body.size(), "got " + std::to_string(response.body.size()) + " bytes");
  });
  co_await check("a body in pieces, whole", [&]() -> async::task<> {
    expect((co_await client.get("/pieces/100")).body == expected_pieces(100), "pieces differ");
  });
  co_await check("a body in pieces, read in pieces", [&]() -> async::task<> {
    http::request request;

    request.target = "/pieces/1000";

    auto response = co_await client.stream(std::move(request));
    std::string all;

    while (auto const piece = co_await response.body.read()) {
      all += *piece;
    }

    expect(all == expected_pieces(1000), "pieces differ");
  });
  co_await check("a response that closes, then another", [&]() -> async::task<> {
    expect((co_await client.get("/close")).body == "bye", "/close");
    expect((co_await client.get("/hello")).body == "hello", "/hello after it");
  });
  // Past the idle timeouts of Node (5 s) and others: the kept connection is closed, which the client sees first.
  co_await check("a GET and a POST after 6 s idle", [&]() -> async::task<> {
    co_await async::sleep_for(std::chrono::seconds{6});
    expect((co_await client.get("/hello")).body == "hello", "GET");
    co_await async::sleep_for(std::chrono::seconds{6});
    expect((co_await client.post("/echo", "late", "text/plain")).body == "late", "POST");
  });
}

async::task<> sse_checks(std::string base) {
  http::client const client{base};

  co_await check("five events", [&]() -> async::task<> {
    http::request request;

    request.target = "/events/5";

    auto response = co_await client.stream(std::move(request));

    expect(response.status == http::status::ok, "status");
    expect(http::is_event_stream(response.fields), "not text/event-stream");

    http::event_reader events{std::move(response.body)};
    auto count = 0;

    while (auto const event = co_await events.next()) {
      expect(event->data == "event " + std::to_string(count), "data \"" + event->data + "\"");
      expect(event->id == std::to_string(count), "id");
      expect(event->type == "count", "type \"" + event->type + "\"");
      ++count;
    }

    expect(count == 5, std::to_string(count) + " events");
  });
}

async::task<> ws_checks(std::string url) {
  co_await check("a WebSocket echo, text and binary", [&]() -> async::task<> {
    auto [socket, handshake] = co_await http::connect_websocket(url);

    co_await socket.send_text("hello");

    auto const text = co_await socket.receive();

    expect(text.opcode == http::websocket_opcode::text && text.data == "hello", "text \"" + text.data + "\"");

    std::string const bytes{"\x00\x01\x02\xff", 4};

    co_await socket.send_binary(bytes);

    auto const binary = co_await socket.receive();

    expect(binary.opcode == http::websocket_opcode::binary && binary.data == bytes, "binary");

    std::string const large(std::size_t{256} * 1024, 'y');

    co_await socket.send_text(large);
    expect((co_await socket.receive()).data == large, "256 KiB");
    co_await socket.close();
  });
}

// Autobahn's fuzzing server: it asks for each case, and the client echoes what the case sends until it closes.
async::task<> autobahn(std::string base) {
  http::websocket_connect_options options;

  options.websocket.max_message_size = std::size_t{64} * 1024 * 1024;

  auto counting = co_await http::connect_websocket(base + "/getCaseCount", options);
  auto const count = std::stoi((co_await counting.socket.receive()).data);

  std::cout << "autobahn: " << count << " cases" << std::endl;

  for (auto index = 1; index <= count; ++index) {
    try {
      auto running =
          co_await http::connect_websocket(base + "/runCase?case=" + std::to_string(index) + "&agent=quoll", options);

      for (;;) {
        auto message = co_await running.socket.receive();

        if (message.opcode == http::websocket_opcode::close) {
          break;
        }

        if (message.opcode == http::websocket_opcode::binary) {
          co_await running.socket.send_binary(message.data);
        } else {
          co_await running.socket.send_text(message.data);
        }
      }
    } catch (std::exception const &failure) {
      // A case that breaks the connection on purpose: what Autobahn judges is in its report.
      std::cout << "case " << index << ": " << failure.what() << std::endl;
    }
  }

  auto reporting = co_await http::connect_websocket(base + "/updateReports?agent=quoll", options);

  try {
    static_cast<void>(co_await reporting.socket.receive());
  } catch (std::exception const &) {
    // It closes once the report is written.
  }
}

async::task<> program(std::vector<std::string> arguments) {
  if (arguments.size() != 2) {
    throw std::invalid_argument{"usage: quoll_interop_client http|sse|ws|autobahn <url>"};
  }

  auto const &what = arguments[0];
  auto const &url = arguments[1];

  if (what == "http") {
    co_await http_checks(url);
  } else if (what == "sse") {
    co_await sse_checks(url);
  } else if (what == "ws") {
    co_await ws_checks(url);
  } else if (what == "autobahn") {
    co_await autobahn(url);
  } else {
    throw std::invalid_argument{"unknown command " + what};
  }

  if (failures > 0) {
    throw std::runtime_error{std::to_string(failures) + " checks failed"};
  }
}

} // namespace

int main(int argc, char **argv) {
  quoll::runtime runtime;

  return runtime.run(program(std::vector<std::string>(argv + 1, argv + argc)));
}
