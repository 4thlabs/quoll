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

#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/client/connect_websocket.hpp>
#include <quoll/client/websocket_refused.hpp>
#include <quoll/http/accept_websocket.hpp>
#include <quoll/http/detail/websocket_key.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/websocket.hpp>
#include <quoll/http/websocket_handshake.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/tls_connector.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/io/write.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/memory_network.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/test/websocket_echo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::websocket_message;
using http::websocket_opcode;
using quoll::async::task;
using quoll::test::memory_connector;
using quoll::test::memory_server;
using namespace std::chrono_literals;

// A server that answers the handshake with `raw`, whatever it asked.
std::function<task<>(io::memory_stream)> answering(std::string raw) {
  return [raw = std::move(raw)](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    static_cast<void>(co_await http::read(stream, buffer, parser));
    (void)co_await io::write(stream, std::as_bytes(std::span{raw}));
  };
}

// What connecting to `server` at `url` threw: its message, empty if nothing.
std::string refusal(memory_server &server, quoll::async::run_loop &loop, std::string url,
                    http::websocket_connect_options options = {}) {
  auto const connecting = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(memory_connector{server}, url, std::move(options)));
    } catch (http::websocket_refused const &refused) {
      co_return std::to_string(static_cast<int>(refused.response().status));
    }

    co_return std::string{};
  };

  return quoll::test::run(loop, connecting()).value();
}

// A connector to a server that never answers: what it is sent is left unread.
class silent_connector {
public:
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    *kept = std::move(server_end);
    co_return io::any_stream{std::move(client_end)};
  }

  std::optional<io::memory_stream> *kept;
};

static_assert(io::Connector<silent_connector>);

// A connector whose connections never open, until they are stopped.
class stuck_connector {
public:
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    co_await quoll::async::sleep_for(24h);
    throw std::logic_error{"connected"};
  }
};

static_assert(io::Connector<stuck_connector>);

// A connector to a memory server that says whether a connection asked for TLS, which it does not do. Its inner
// connector is a member: a coroutine called on a temporary would read it once that has gone.
class tls_memory_connector {
public:
  tls_memory_connector(memory_server &server, bool &used_tls, std::string *trusted = nullptr) noexcept
      : inner_(server), used_tls_(&used_tls), trusted_(trusted) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return inner_.connect(std::move(host), port);
  }

  [[nodiscard]] task<io::any_stream> connect_tls(std::string host, std::uint16_t port,
                                                 io::tls_client_options const &tls) const {
    *used_tls_ = true;

    // What a copy of it, as connect_websocket keeps, was asked to trust.
    if (trusted_ != nullptr) {
      *trusted_ = tls.ca_file;
    }

    return inner_.connect(std::move(host), port);
  }

private:
  memory_connector inner_;
  bool *used_tls_;
  std::string *trusted_;
};

static_assert(io::TlsConnector<tls_memory_connector>);

// A connector that moves and does not copy, as one holding what it alone owns does.
class move_only_connector {
public:
  explicit move_only_connector(memory_server &server) : inner_(std::make_unique<memory_connector>(server)) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    return inner_->connect(std::move(host), port);
  }

private:
  std::unique_ptr<memory_connector> inner_;
};

static_assert(io::Connector<move_only_connector>);
static_assert(!std::copy_constructible<move_only_connector>);

// A server that answers the handshake with 103, then its 101, right.
task<> early_hints_then_switch(io::memory_stream stream) {
  std::string buffer;
  http::request_parser parser;

  static_cast<void>(co_await http::read(stream, buffer, parser));

  auto const request = parser.release();
  auto const answer = "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " +
                      http::websocket_accept(request.fields.find("Sec-WebSocket-Key").value_or("")) + "\r\n\r\n";

  (void)co_await io::write(stream, std::as_bytes(std::span{answer}));
}

// A server that answers the handshake with `before`, then with `answer`, `{accept}` in it replaced with the
// Sec-WebSocket-Accept the handshake's key calls for.
std::function<task<>(io::memory_stream)> answering_with_accept(std::string before, std::string answer) {
  return [before = std::move(before), answer = std::move(answer)](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    static_cast<void>(co_await http::read(stream, buffer, parser));

    auto const request = parser.release();
    auto const accept = http::websocket_accept(request.fields.find("Sec-WebSocket-Key").value_or(""));
    auto wire = before + answer;

    // Each one, as a test of two accepts names it twice.
    for (auto at = wire.find("{accept}"); at != std::string::npos; at = wire.find("{accept}")) {
      wire.replace(at, std::string_view{"{accept}"}.size(), accept);
    }

    (void)co_await io::write(stream, std::as_bytes(std::span{wire}));
  };
}

// A stream that cannot write, a peer that refused as it read, and reads what that peer sent before it went.
class unwritable {
public:
  explicit unwritable(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    co_return io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
  }

private:
  io::memory_stream inner_;
};

// A stream whose writes fail on this side, with the peer still there and silent.
class failing_writes {
public:
  explicit failing_writes(io::memory_stream inner) noexcept : inner_(std::move(inner)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const>) {
    co_return io::io_result{std::make_error_code(std::errc::io_error), 0};
  }

private:
  io::memory_stream inner_;
};

// A connector whose streams cannot write, to a peer kept in `kept`.
class failing_connector {
public:
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    *kept = std::move(server_end);
    co_return io::any_stream{failing_writes{std::move(client_end)}};
  }

  std::optional<io::memory_stream> *kept;
};

static_assert(io::Connector<failing_connector>);

// A server that keeps the handshake it was sent in `sent`, and refuses it.
std::function<task<>(io::memory_stream)> recording(http::request &sent) {
  return [&sent](io::memory_stream stream) -> task<> {
    std::string buffer;
    http::request_parser parser;

    static_cast<void>(co_await http::read(stream, buffer, parser));
    sent = parser.release();

    auto const refusal = std::string{"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"};

    (void)co_await io::write(stream, std::as_bytes(std::span{refusal}));
  };
}

// A stream that takes nothing of a write, broken by the peer as it reads: the 101 the handshake it was given calls
// for, then gone, if `answers`; gone with nothing otherwise.
class broken_on_write {
public:
  broken_on_write(io::memory_stream inner, io::memory_stream peer, bool answers) noexcept
      : inner_(std::move(inner)), peer_(std::move(peer)), answers_(answers) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (peer_ && answers_) {
      auto const sent = std::string_view{reinterpret_cast<char const *>(data.data()), data.size()};
      auto const name = std::string_view{"Sec-WebSocket-Key: "};
      auto const start = sent.find(name) + name.size();
      auto const key = sent.substr(start, sent.find("\r\n", start) - start);
      auto const answer = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " +
                          http::websocket_accept(key) + "\r\n\r\n";

      (void)co_await io::write(*peer_, std::as_bytes(std::span{answer}));
    }

    peer_.reset();
    co_return io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
  }

private:
  io::memory_stream inner_;
  std::optional<io::memory_stream> peer_;
  bool answers_;
};

// A connector whose streams break as the handshake is written: see broken_on_write.
class breaking_connector {
public:
  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    co_return io::any_stream{broken_on_write{std::move(client_end), std::move(server_end), answers}};
  }

  bool answers;
};

static_assert(io::Connector<breaking_connector>);

// A stream that writes only while what owns it, its connector, is there: one that points into its connector.
class owned_by_connector {
public:
  owned_by_connector(io::memory_stream inner, std::weak_ptr<bool const> connector) noexcept
      : inner_(std::move(inner)), connector_(std::move(connector)) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) { return inner_.read_some(buffer); }

  [[nodiscard]] task<io::io_result> write_some(std::span<std::byte const> data) {
    if (connector_.expired()) {
      co_return io::io_result{std::make_error_code(std::errc::bad_file_descriptor), 0};
    }

    co_return co_await inner_.write_some(data);
  }

private:
  io::memory_stream inner_;
  std::weak_ptr<bool const> connector_;
};

// A connector to `server`, whose streams point into it.
class owning_connector {
public:
  explicit owning_connector(memory_server &server) : inner_(server), alive_(std::make_shared<bool const>(true)) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    auto stream = co_await inner_.connect(std::move(host), port);
    auto *const memory = stream.target<io::memory_stream>();

    co_return io::any_stream{owned_by_connector{std::move(*memory), alive_}};
  }

private:
  memory_connector inner_;
  std::shared_ptr<bool const> alive_;
};

static_assert(io::Connector<owning_connector>);

// A connector to a peer that answered `answer` and went, before reading anything.
class refusing_connector {
public:
  explicit refusing_connector(std::string answer) : answer_(std::move(answer)) {}

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    (void)co_await io::write(server_end, std::as_bytes(std::span{answer_}));
    co_return io::any_stream{unwritable{std::move(client_end)}};
  }

private:
  std::string answer_;
};

static_assert(io::Connector<refusing_connector>);

} // namespace

TEST_CASE("connect_websocket: opens a WebSocket to a server, which answers what it sends", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string target;
  http::router router;

  router.get("/room", [&target](http::request request) {
    target = request.target;
    return http::accept_websocket(request, quoll::test::websocket_echo);
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  auto const talking = [&]() -> task<int> {
    auto [socket, handshake] =
        co_await http::connect_websocket(memory_connector{server}, "ws://chat.example/room?id=4");

    co_await socket.send_text("hello");

    auto const echoed = co_await socket.receive();

    co_await socket.close();

    auto const closed = co_await socket.receive();

    REQUIRE(handshake.status == http::status::switching_protocols);
    REQUIRE(echoed == websocket_message{websocket_opcode::text, "hello"});
    REQUIRE(closed == websocket_message{websocket_opcode::close, "", 1000});
    co_return 1;
  };

  // The server takes only masked frames: what it echoed came from a client's end.
  REQUIRE(quoll::test::run(loop, talking()) == 1);
  REQUIRE(target == "/room?id=4");
  REQUIRE(server.destination == "chat.example:80");
}

TEST_CASE("connect_websocket: a subprotocol chosen is one offered", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string chosen;
  http::router router;

  router.get("/", [&chosen](http::request request) {
    auto response = http::accept_websocket(request, quoll::test::websocket_echo);

    response.fields.set("Sec-WebSocket-Protocol", chosen);
    return response;
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};
  http::websocket_connect_options options;

  options.fields.set("Sec-WebSocket-Protocol", "chat, superchat");

  SECTION("one offered") {
    chosen = "superchat";

    auto const connecting = [&]() -> task<std::string> {
      auto connected = co_await http::connect_websocket(memory_connector{server}, "ws://h/", options);

      co_await connected.socket.close();
      static_cast<void>(co_await connected.socket.receive());
      co_return std::string{connected.handshake.fields.find("Sec-WebSocket-Protocol").value_or("")};
    };

    REQUIRE(quoll::test::run(loop, connecting()) == "superchat");
  }

  SECTION("one not offered is refused") {
    chosen = "other";

    REQUIRE(refusal(server, loop, "ws://h/", options) == "101");
  }
}

TEST_CASE("connect_websocket: what is no switch to the WebSocket asked for is refused, with its answer",
          "[client][websocket]") {
  quoll::async::run_loop loop;

  SECTION("another status") {
    http::router router;

    router.get("/", [](http::request) {
      http::response response;

      response.status = http::status::forbidden;
      return response;
    });

    memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "403");
  }

  SECTION("a 101 with another accept") {
    memory_server server{loop.get_executor(),
                         answering("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
                         0,
                         {},
                         {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "101");
  }

  SECTION("a 101 with an extension none offered") {
    http::router router;

    router.get("/", [](http::request request) {
      auto response = http::accept_websocket(request, quoll::test::websocket_echo);

      response.fields.set("Sec-WebSocket-Extensions", "permessage-deflate");
      return response;
    });

    memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

    REQUIRE(refusal(server, loop, "ws://h/") == "101");
  }
}

TEST_CASE("connect_websocket: what cannot be asked is refused when it is called", "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(), answering(""), 0, {}, {}};
  http::websocket_connect_options reserved;

  reserved.fields.set("Sec-WebSocket-Key", "mine");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "http://h/"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/#part"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/?a b"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", reserved), std::invalid_argument);

  http::websocket_connect_options no_token;

  no_token.fields.set("Sec-WebSocket-Protocol", "chat, my chat");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", no_token), std::invalid_argument);

  http::websocket_connect_options twice_in_two;

  // Across two fields too.
  twice_in_two.fields.add("Sec-WebSocket-Protocol", "chat");
  twice_in_two.fields.add("Sec-WebSocket-Protocol", "chat");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", twice_in_two), std::invalid_argument);

  http::websocket_connect_options with_a_body;

  // Chunks, which the serializer would write after the head.
  with_a_body.fields.set("Transfer-Encoding", "chunked");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", with_a_body), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "wss://h/"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://bad host/"), std::invalid_argument);
  // What a proxy could read as another server, as it is, decoded, without its dot segments, or with a backslash.
  for (auto const *const url : {"ws://h//evil.example/x", "ws://h/%2F/evil.example/x", "ws://h/.//evil.example/x",
                                "ws://h/./%2Fevil.example", "ws://h/\\evil.example"}) {
    REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, url), std::invalid_argument);
  }

  http::websocket_connect_options no_subprotocol;

  no_subprotocol.fields.set("Sec-WebSocket-Protocol", " , ");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", no_subprotocol),
                    std::invalid_argument);

  http::websocket_connect_options twice;

  twice.fields.set("Sec-WebSocket-Protocol", "chat, chat");

  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", twice), std::invalid_argument);

  http::websocket_connect_options broken;

  broken.fields.set("X-Broken", "a\r\nb");

  // Refused before any connection, as it would not serialize.
  REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", broken), std::invalid_argument);
  REQUIRE(server.connections == 0);
}

TEST_CASE("connect_websocket: a server that does not answer the handshake in time", "[client][websocket]") {
  quoll::test::virtual_time time;
  std::optional<io::memory_stream> kept;
  http::websocket_connect_options options;

  options.handshake_timeout = 1s;

  auto const connecting = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(
        http::connect_websocket(silent_connector{&kept}, "ws://h/", std::move(options)));
  };

  REQUIRE(quoll::test::run(time, connecting()) == std::errc::timed_out);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("connect_websocket: what the server sends right behind its 101 is the first message", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::router router;

  router.get("/", [](http::request request) {
    return http::accept_websocket(request, [](http::websocket socket) -> task<> {
      co_await socket.send_text("first");

      while ((co_await socket.receive()).opcode != websocket_opcode::close) {
      }
    });
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  auto const receiving = [&]() -> task<websocket_message> {
    auto connected = co_await http::connect_websocket(memory_connector{server}, "ws://h/");
    auto first = co_await connected.socket.receive();

    co_await connected.socket.close();
    static_cast<void>(co_await connected.socket.receive());
    co_return first;
  };

  REQUIRE(quoll::test::run(loop, receiving()) == websocket_message{websocket_opcode::text, "first"});
}

TEST_CASE("connect_websocket: wss connects with TLS, on 443", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::router router;
  bool used_tls = false;

  router.get("/", [](http::request request) { return http::accept_websocket(request, quoll::test::websocket_echo); });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  auto const connecting = [&]() -> task<int> {
    auto connected = co_await http::connect_websocket(tls_memory_connector{server, used_tls}, "wss://h/");

    co_await connected.socket.close();
    static_cast<void>(co_await connected.socket.receive());
    co_return 1;
  };

  REQUIRE(quoll::test::run(loop, connecting()) == 1);
  REQUIRE(used_tls);
  REQUIRE(server.destination == "h:443");
}

TEST_CASE("connect_websocket: interim answers before the 101 are skipped, through a connector that only moves",
          "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(), early_hints_then_switch, 0, {}, {}};

  auto const connecting = [&]() -> task<http::status> {
    auto connected = co_await http::connect_websocket(move_only_connector{server}, "ws://h/");

    co_return connected.handshake.status;
  };

  REQUIRE(quoll::test::run(loop, connecting()) == http::status::switching_protocols);
}

TEST_CASE("connect_websocket: by URL alone, through the executor's network", "[client][websocket]") {
  // A run_loop offers none: the overload goes to it all the same.
  auto const connecting = []() -> task<int> {
    static_cast<void>(co_await http::connect_websocket("ws://h/"));
    co_return 1;
  };

  REQUIRE_THROWS_AS(quoll::test::run(connecting()), std::logic_error);
}

TEST_CASE("connect_websocket: each handshake has a key of its own", "[client][websocket]") {
  auto const first = http::detail::random_websocket_key();
  auto const second = http::detail::random_websocket_key();
  http::request handshake;

  handshake.fields.set("Host", "h");
  handshake.fields.set("Upgrade", "websocket");
  handshake.fields.set("Connection", "Upgrade");
  handshake.fields.set("Sec-WebSocket-Version", "13");
  handshake.fields.set("Sec-WebSocket-Key", first);

  // 16 bytes in base64, as a server reads them.
  REQUIRE(http::is_websocket_upgrade(handshake));
  REQUIRE(first != second);
}

TEST_CASE("connect_websocket: a refusal that came as the handshake could not be sent is the one thrown",
          "[client][websocket]") {
  quoll::async::run_loop loop;

  auto const connecting = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(
          refusing_connector{"HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer\r\nContent-Length: 0\r\n\r\n"},
          "ws://h/"));
    } catch (http::websocket_refused const &refused) {
      co_return std::string{refused.response().fields.find("WWW-Authenticate").value_or("")};
    }

    co_return std::string{};
  };

  REQUIRE(quoll::test::run(loop, connecting()) == "Bearer");
}

TEST_CASE("connect_websocket: interim answers, up to eight", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string hints;
  auto count = 0;
  std::error_code expected;

  SECTION("eight") { count = 8; }

  SECTION("nine are too many") {
    count = 9;
    expected = http::error::bad_message;
  }

  for (auto hint = 0; hint < count; ++hint) {
    hints += "HTTP/1.1 103 Early Hints\r\n\r\n";
  }

  memory_server server{loop.get_executor(),
                       answering_with_accept(hints, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                    "Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"),
                       0,
                       {},
                       {}};

  auto const connecting = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(http::connect_websocket(memory_connector{server}, "ws://h/"));
  };

  REQUIRE(quoll::test::run(loop, connecting()) == expected);
}

TEST_CASE("connect_websocket: a 101 that does not name the WebSocket is refused", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string answer;

  SECTION("no Upgrade") {
    answer = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n";
  }

  SECTION("no Connection: Upgrade") {
    answer = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: {accept}\r\n\r\n";
  }

  memory_server server{loop.get_executor(), answering_with_accept({}, answer), 0, {}, {}};

  REQUIRE(refusal(server, loop, "ws://h/") == "101");
}

TEST_CASE("connect_websocket: a write that fails on this side is thrown, not waited past", "[client][websocket]") {
  std::optional<io::memory_stream> kept;

  auto const connecting = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(http::connect_websocket(failing_connector{&kept}, "ws://h/"));
  };

  // No timer on a run_loop: waiting for an answer would never end.
  REQUIRE(quoll::test::run(connecting()) == std::errc::io_error);
}

TEST_CASE("connect_websocket: the handshake as it is sent", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::request sent;
  memory_server server{loop.get_executor(), recording(sent), 0, {}, {}};
  http::websocket_connect_options options;

  options.fields.set("Origin", "https://app.example");

  // A target the serializer takes, encoded slash and empty segment included, as no path prefix is.
  REQUIRE(refusal(server, loop, "ws://h:8080/files/a%2Fb//c?x=1", options) == "400");
  REQUIRE(sent.target == "/files/a%2Fb//c?x=1");
  REQUIRE(sent.fields.find("Host") == "h:8080");
  REQUIRE(sent.fields.find("Origin") == "https://app.example");
  REQUIRE(sent.fields.find("Sec-WebSocket-Version") == "13");
  REQUIRE(sent.fields.find("Upgrade") == "websocket");
  REQUIRE(sent.fields.find("Connection") == "Upgrade");
  REQUIRE(sent.fields.find("Sec-WebSocket-Key").value_or("").size() == 24);
}

TEST_CASE("connect_websocket: a subprotocol the server names empty is refused", "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(),
                       answering_with_accept({}, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n"
                                                 "Sec-WebSocket-Protocol: \r\n\r\n"),
                       0,
                       {},
                       {}};
  http::websocket_connect_options options;

  options.fields.set("Sec-WebSocket-Protocol", "chat,");

  REQUIRE(refusal(server, loop, "ws://h/", options) == "101");
}

TEST_CASE("connect_websocket: a refusal is judged by its head, its body left unread", "[client][websocket]") {
  quoll::async::run_loop loop;
  // No length: its body would end with a connection the server keeps open.
  memory_server server{loop.get_executor(),
                       [](io::memory_stream stream) -> task<> {
                         std::string buffer;
                         http::request_parser parser;

                         static_cast<void>(co_await http::read(stream, buffer, parser));

                         auto const refusal = std::string{"HTTP/1.1 403 Forbidden\r\n\r\nno end"};

                         (void)co_await io::write(stream, std::as_bytes(std::span{refusal}));

                         // Open for as long as the client is there: it sends nothing more.
                         std::array<char, 16> rest{};

                         static_cast<void>(co_await stream.read_some(std::as_writable_bytes(std::span{rest})));
                       },
                       0,
                       {},
                       {}};

  REQUIRE(refusal(server, loop, "ws://h/") == "403");

  // Its head alone, whatever of the body came with it.
  auto const body = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(memory_connector{server}, "ws://h/"));
    } catch (http::websocket_refused const &refused) {
      co_return refused.response().body;
    }

    co_return std::string{"connected"};
  };

  REQUIRE(quoll::test::run(loop, body()).value().empty());
}

TEST_CASE("connect_websocket: what a 101 may not name, or name twice", "[client][websocket]") {
  quoll::async::run_loop loop;
  std::string answer;
  http::websocket_connect_options options;

  SECTION("a subprotocol, none offered") { answer = "Sec-WebSocket-Protocol: chat\r\n"; }

  SECTION("two subprotocols") {
    options.fields.set("Sec-WebSocket-Protocol", "chat, other");
    answer = "Sec-WebSocket-Protocol: chat\r\nSec-WebSocket-Protocol: other\r\n";
  }

  SECTION("two accepts, both right") { answer = "Sec-WebSocket-Accept: {accept}\r\n"; }

  SECTION("an Upgrade to another protocol") { answer = "Upgrade: h2c\r\n"; }

  memory_server server{loop.get_executor(),
                       answering_with_accept({}, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n" +
                                                     answer + "\r\n"),
                       0,
                       {},
                       {}};

  REQUIRE(refusal(server, loop, "ws://h/", options) == "101");
}

TEST_CASE("connect_websocket: each handshake sends a key of its own, and Host as the URL names it",
          "[client][websocket]") {
  quoll::async::run_loop loop;
  http::request first;
  http::request second;
  memory_server to_first{loop.get_executor(), recording(first), 0, {}, {}};
  memory_server to_second{loop.get_executor(), recording(second), 0, {}, {}};

  REQUIRE(refusal(to_first, loop, "ws://h/") == "400");
  REQUIRE(refusal(to_second, loop, "ws://[::1]:8080/") == "400");
  REQUIRE(first.fields.find("Sec-WebSocket-Key") != second.fields.find("Sec-WebSocket-Key"));
  REQUIRE(second.fields.find("Host") == "[::1]:8080");
}

TEST_CASE("connect_websocket: what is refused in a URL says so for connect_websocket", "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(), answering(""), 0, {}, {}};

  for (auto const *const url : {"ws://h:99999/", "ws://bad host/", "ws:///path"}) {
    try {
      static_cast<void>(http::connect_websocket(memory_connector{server}, url));
      FAIL("refused");
    } catch (std::invalid_argument const &refused) {
      REQUIRE(std::string_view{refused.what()}.starts_with("quoll::http::connect_websocket: "));
    }
  }
}

TEST_CASE("connect_websocket: the handshake's limit bounds connecting too", "[client][websocket]") {
  quoll::test::virtual_time time;
  http::websocket_connect_options options;

  options.handshake_timeout = 1s;

  auto const connecting = [&]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(
        http::connect_websocket(stuck_connector{}, "ws://h/", std::move(options)));
  };

  REQUIRE(quoll::test::run(time, connecting()) == std::errc::timed_out);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("connect_websocket: wss on its default port names no port in Host", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::request sent;
  memory_server server{loop.get_executor(), recording(sent), 0, {}, {}};
  bool used_tls = false;

  auto const connecting = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(tls_memory_connector{server, used_tls}, "wss://h/"));
    } catch (http::websocket_refused const &) {
      co_return std::string{sent.fields.find("Host").value_or("")};
    }

    co_return std::string{};
  };

  REQUIRE(quoll::test::run(loop, connecting()) == "h");
  REQUIRE(used_tls);
}

TEST_CASE("connect_websocket: a handshake that could not be sent says so", "[client][websocket]") {
  auto answers = false;

  // Its 101 is no connection to use; with nothing come, the write's error says more than the end of the stream.
  SECTION("a 101 came all the same") { answers = true; }

  SECTION("nothing came") { answers = false; }

  auto const connecting = [answers]() -> task<std::error_code> {
    co_return co_await quoll::test::code_thrown_by(http::connect_websocket(breaking_connector{answers}, "ws://h/"));
  };

  REQUIRE(quoll::test::run(connecting()) == std::errc::broken_pipe);
}

TEST_CASE("connect_websocket: each field of the handshake's own is refused", "[client][websocket]") {
  quoll::async::run_loop loop;
  memory_server server{loop.get_executor(), answering(""), 0, {}, {}};

  for (auto const *const name :
       {"Host", "Upgrade", "Connection", "Sec-WebSocket-Key", "Sec-WebSocket-Version", "Sec-WebSocket-Extensions",
        "Sec-WebSocket-Accept", "Content-Length", "Transfer-Encoding"}) {
    http::websocket_connect_options options;

    options.fields.set(name, "1");

    REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", options), std::invalid_argument);

    // Whatever its case.
    http::websocket_connect_options lower;
    auto lower_name = std::string{name};

    std::ranges::transform(lower_name, lower_name.begin(), [](char letter) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    });
    lower.fields.add(lower_name, "1");

    REQUIRE_THROWS_AS(http::connect_websocket(memory_connector{server}, "ws://h/", lower), std::invalid_argument);
  }
}

TEST_CASE("connect_websocket: its options reach what they say", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::router router;

  router.get("/", [](http::request request) {
    return http::accept_websocket(request, [](http::websocket socket) -> task<> {
      co_await socket.send_text("12345");

      while ((co_await socket.receive()).opcode != websocket_opcode::close) {
      }
    });
  });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};
  http::websocket_connect_options options;

  SECTION("websocket: the most a message holds") {
    options.websocket.max_message_size = 4;

    auto const receiving = [&]() -> task<std::error_code> {
      auto connected = co_await http::connect_websocket(memory_connector{server}, "ws://h/", std::move(options));

      co_return co_await quoll::test::code_thrown_by(connected.socket.receive());
    };

    REQUIRE(quoll::test::run(loop, receiving()) == http::error::websocket_too_large);
  }

  SECTION("limits: the size of the server's answer") {
    options.limits.max_header_size = 32;

    auto const connecting = [&]() -> task<std::error_code> {
      co_return co_await quoll::test::code_thrown_by(
          http::connect_websocket(memory_connector{server}, "ws://h/", std::move(options)));
    };

    REQUIRE(quoll::test::run(loop, connecting()) == http::error::header_too_large);
  }

  SECTION("tls: what the connection trusts") {
    bool used_tls = false;
    std::string trusted;

    options.tls.ca_file = "authorities.pem";

    auto const connecting = [&]() -> task<int> {
      auto connected = co_await http::connect_websocket(tls_memory_connector{server, used_tls, &trusted}, "wss://h/",
                                                        std::move(options));

      co_await connected.socket.close();
      static_cast<void>(co_await connected.socket.receive());
      co_return 1;
    };

    REQUIRE(quoll::test::run(loop, connecting()) == 1);
    REQUIRE(trusted == "authorities.pem");
  }
}

TEST_CASE("connect_websocket: a handshake_timeout of zero waits as long as the server takes", "[client][websocket]") {
  quoll::test::virtual_time time;
  http::websocket_connect_options options;

  options.handshake_timeout = 0s;

  // A server that answers, refusing, after a minute.
  struct late_connector {
    [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
      auto [client_end, server_end] = io::make_memory_stream_pair();
      auto const refusal = std::string{"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"};

      co_await quoll::async::sleep_for(60s);
      (void)co_await io::write(server_end, std::as_bytes(std::span{refusal}));
      *kept = std::move(server_end);
      co_return io::any_stream{std::move(client_end)};
    }

    std::optional<io::memory_stream> *kept;
  };

  std::optional<io::memory_stream> kept;

  auto const connecting = [&]() -> task<std::string> {
    try {
      static_cast<void>(co_await http::connect_websocket(late_connector{&kept}, "ws://h/", std::move(options)));
    } catch (http::websocket_refused const &refused) {
      co_return std::to_string(static_cast<int>(refused.response().status));
    }

    co_return std::string{};
  };

  REQUIRE(quoll::test::run(time, connecting()) == "403");
  REQUIRE(time.now() == 60s);
}

TEST_CASE("connect_websocket: the connector lives as long as the stream it opened", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::router router;

  router.get("/", [](http::request request) { return http::accept_websocket(request, quoll::test::websocket_echo); });

  memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  // Its streams write only while it is there: past the handshake, the WebSocket's sends need it.
  auto const talking = [&]() -> task<websocket_message> {
    auto connected = co_await http::connect_websocket(owning_connector{server}, "ws://h/");

    co_await connected.socket.send_text("still there");

    auto answer = co_await connected.socket.receive();

    co_await connected.socket.close();
    static_cast<void>(co_await connected.socket.receive());
    co_return answer;
  };

  REQUIRE(quoll::test::run(loop, talking()) == websocket_message{websocket_opcode::text, "still there"});
}

TEST_CASE("connect_websocket: the subprotocols offered go out as checked", "[client][websocket]") {
  quoll::async::run_loop loop;
  http::request sent;
  memory_server server{loop.get_executor(), recording(sent), 0, {}, {}};
  http::websocket_connect_options options;

  options.fields.set("Sec-WebSocket-Protocol", "chat,, superchat ,");

  REQUIRE(refusal(server, loop, "ws://h/", options) == "400");
  REQUIRE(sent.fields.find("Sec-WebSocket-Protocol") == "chat, superchat");
}
