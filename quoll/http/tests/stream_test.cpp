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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/test/check.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using quoll::async::task;
using quoll::test::check;

using quoll::test::overloaded;
using quoll::test::run;

std::span<std::byte const> bytes(std::string_view text) { return std::as_bytes(std::span{text}); }

template <io::ByteStream Stream>
task<http::response> get(Stream &stream, std::string target) {
  http::request request;

  request.method = http::verb::get;
  request.target = std::move(target);
  request.fields.add("Host", "example.test");
  check(co_await http::write(stream, request));

  std::string buffer;
  http::response_parser parser;

  check(co_await http::read(stream, buffer, parser));
  co_return parser.release();
}

// Answers one request with a JSON body, and hands the request over for inspection.
template <io::ByteStream Stream>
task<http::request> serve_one(Stream &stream) {
  std::string buffer;
  http::request_parser parser;

  check(co_await http::read(stream, buffer, parser));

  auto request = parser.release();
  http::response response;

  response.status = http::status::ok;
  response.fields.add("Content-Type", "application/json");
  response.body = R"({"id":42})";
  check(co_await http::write(stream, response));
  co_return request;
}

// Reads the request, then fails before answering.
task<> fail_to_serve(io::memory_stream &stream) {
  std::string buffer;
  http::request_parser parser;

  check(co_await http::read(stream, buffer, parser));
  throw std::runtime_error{"server failed"};
}

task<std::tuple<http::response, std::monostate>> get_from_failing_server(io::memory_stream &client,
                                                                         io::memory_stream &server) {
  co_return co_await quoll::async::when_all(get(client, "/accounts/42"), fail_to_serve(server));
}

// Reads a protocol switch, then tries to read an HTTP message again.
task<std::tuple<std::error_code, std::error_code, std::string>> read_past_upgrade(io::memory_stream &stream) {
  std::string buffer;
  http::response_parser parser;
  auto const first = co_await http::read(stream, buffer, parser);

  if (first || !parser.upgrade()) {
    co_return std::tuple{first, std::error_code{}, buffer};
  }

  (void)parser.release();

  auto const second = co_await http::read(stream, buffer, parser);

  co_return std::tuple{first, second, buffer};
}

template <io::ByteStream Stream>
task<std::tuple<http::response, http::request>> get_from_server(Stream &client, Stream &server) {
  co_return co_await quoll::async::when_all(get(client, "/accounts/42"), serve_one(server));
}

// Sends `wire` one byte at a time, letting the reader run between them, then closes when asked to.
task<> trickle(io::memory_stream &stream, std::string_view wire, bool close) {
  for (std::size_t index = 0; index < wire.size(); ++index) {
    check((co_await io::write(stream, bytes(wire.substr(index, 1)))).error);
    co_await quoll::async::yield();
  }

  if (close) {
    stream.close();
  }
}

// What a read produced: its error, and the message when there is one.
template <class Message>
struct read_outcome {
  std::error_code error;
  std::optional<Message> message;
};

template <class Message>
task<read_outcome<Message>> read_message(io::memory_stream &stream, std::string &buffer,
                                         http::parser<Message> &parser) {
  auto const error = co_await http::read(stream, buffer, parser);

  if (error || !parser.done()) {
    co_return read_outcome<Message>{error, std::nullopt};
  }

  co_return read_outcome<Message>{error, parser.release()};
}

task<std::tuple<read_outcome<http::response>, std::monostate>>
read_trickled(io::memory_stream &reader, io::memory_stream &writer, std::string_view wire, bool close) {
  std::string buffer;
  http::response_parser parser;

  co_return co_await quoll::async::when_all(read_message(reader, buffer, parser), trickle(writer, wire, close));
}

task<read_outcome<http::response>> read_response(io::memory_stream &stream) {
  std::string buffer;
  http::response_parser parser;

  co_return co_await read_message(stream, buffer, parser);
}

task<std::error_code> write_request(io::memory_stream &stream, http::request request) {
  co_return co_await http::write(stream, request);
}

task<std::vector<std::string>> read_targets(io::memory_stream &stream, std::string &buffer) {
  std::vector<std::string> targets;
  http::request_parser parser;

  for (;;) {
    auto const error = co_await http::read(stream, buffer, parser);

    if (error == io::error::end_of_stream) {
      co_return targets;
    }

    check(error);
    targets.push_back(parser.release().target);
  }
}

} // namespace

TEST_CASE("http over a stream: a client GET, end to end", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  auto const result = run(get_from_server(client, server));

  REQUIRE(result);

  auto const &[response, request] = *result;

  REQUIRE(request.method == http::verb::get);
  REQUIRE(request.target == "/accounts/42");
  REQUIRE(request.fields.find("host") == "example.test");
  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.fields.find("content-type") == "application/json");
  REQUIRE(response.fields.find("content-length") == "9");
  REQUIRE(response.body == R"({"id":42})");
}

TEST_CASE("http over a stream: a client GET over type-erased streams", "[http][stream]") {
  auto [left, right] = io::make_memory_stream_pair();
  io::any_stream client{std::move(left)};
  io::any_stream server{std::move(right)};

  auto const result = run(get_from_server(client, server));

  REQUIRE(result);

  auto const &[response, request] = *result;

  REQUIRE(request.target == "/accounts/42");
  REQUIRE(response.body == R"({"id":42})");
}

TEST_CASE("http over a stream: a failing server stops the client's read", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  REQUIRE_THROWS_AS(run(get_from_failing_server(client, server)), std::runtime_error);
}

TEST_CASE("http over a stream: no HTTP is read past a protocol switch", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  REQUIRE(!run(io::write(server, bytes("HTTP/1.1 101 Switching Protocols\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Upgrade: websocket\r\n"
                                       "\r\n"
                                       "\x81\x05")))
               ->error);

  auto const result = run(read_past_upgrade(client));

  REQUIRE(result);

  auto const &[first, second, left_over] = *result;

  REQUIRE(!first);
  REQUIRE(second == http::error::upgraded);
  REQUIRE(left_over == "\x81\x05");
}

TEST_CASE("http over a stream: a response arriving byte by byte is read whole", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  auto const result = run(read_trickled(client, server, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", false));

  REQUIRE(result);

  auto const &outcome = std::get<0>(*result);

  REQUIRE(!outcome.error);
  REQUIRE(outcome.message);
  REQUIRE(outcome.message->body == "hello");
}

TEST_CASE("http over a stream: a body delimited by the end of the stream", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  auto const result = run(read_trickled(client, server, "HTTP/1.1 200 OK\r\n\r\nuntil close", true));

  REQUIRE(result);

  auto const &outcome = std::get<0>(*result);

  REQUIRE(!outcome.error);
  REQUIRE(outcome.message);
  REQUIRE(outcome.message->body == "until close");
}

TEST_CASE("http over a stream: a stream closed mid-message is a partial message", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  auto const result = run(read_trickled(client, server, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", true));

  REQUIRE(result);
  REQUIRE(std::get<0>(*result).error == http::error::partial_message);
}

TEST_CASE("http over a stream: malformed input reports the parser error", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  auto const result = run(read_trickled(client, server, "NOT HTTP AT ALL\r\n\r\n", false));

  REQUIRE(result);
  REQUIRE(std::get<0>(*result).error == http::error::bad_message);
}

TEST_CASE("http over a stream: a stream closed between messages is the end of the stream", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  server.close();

  auto const outcome = run(read_response(client));

  REQUIRE(outcome);
  REQUIRE(outcome->error == io::error::end_of_stream);
}

TEST_CASE("http over a stream: pipelined requests are read one at a time", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  REQUIRE(!run(io::write(client, bytes("GET /first HTTP/1.1\r\nHost: a\r\n\r\n"
                                       "GET /second HTTP/1.1\r\nHost: a\r\n\r\n")))
               ->error);
  client.close();

  std::string buffer;
  auto const targets = run(read_targets(server, buffer));

  REQUIRE(targets);
  REQUIRE(*targets == std::vector<std::string>{"/first", "/second"});
  REQUIRE(buffer.empty());
}

TEST_CASE("http over a stream: an invalid message throws when write() is called", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();
  http::request request;

  request.fields.add("bad name", "value");

  REQUIRE_THROWS_AS((void)http::write(client, request), std::invalid_argument);
}

TEST_CASE("http over a stream: writing to a peer that is gone fails", "[http][stream]") {
  auto [client, server] = io::make_memory_stream_pair();

  { auto gone = std::move(server); }

  auto const error = run(write_request(client, http::request{}));

  REQUIRE(error);
  REQUIRE(*error == std::errc::broken_pipe);
}
