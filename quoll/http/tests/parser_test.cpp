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

#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace http = quoll::http;

std::span<char const> bytes(std::string_view text) { return {text.data(), text.size()}; }

// Feeds `wire` one byte at a time, as a slow peer would.
template <class Parser>
http::parse_result feed_bytewise(Parser &parser, std::string_view wire) {
  std::size_t consumed = 0;

  while (consumed < wire.size() && !parser.done()) {
    auto const result = parser.feed(bytes(wire.substr(consumed, 1)));

    if (result.error) {
      return {consumed + result.consumed, result.error};
    }

    consumed += result.consumed;
  }

  return {consumed, {}};
}

constexpr std::string_view simple_response = "HTTP/1.1 200 OK\r\n"
                                             "Content-Type: text/plain\r\n"
                                             "Content-Length: 5\r\n"
                                             "\r\n"
                                             "hello";

} // namespace

TEST_CASE("A response with Content-Length is parsed", "[http][parser]") {
  http::response_parser parser;

  auto const result = parser.feed(bytes(simple_response));

  REQUIRE_FALSE(result.error);
  REQUIRE(result.consumed == simple_response.size());
  REQUIRE(parser.done());
  REQUIRE(parser.keep_alive());

  auto const response = parser.release();

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.reason == "OK");
  REQUIRE(response.version == http::version::http_1_1);
  REQUIRE(response.fields.find("content-type") == "text/plain");
  REQUIRE(response.body == "hello");
}

TEST_CASE("A response fed one byte at a time is parsed", "[http][parser]") {
  http::response_parser parser;

  auto const result = feed_bytewise(parser, simple_response);

  REQUIRE_FALSE(result.error);
  REQUIRE(result.consumed == simple_response.size());
  REQUIRE(parser.done());
  REQUIRE(parser.release().body == "hello");
}

TEST_CASE("A chunked body is joined and its trailers kept apart", "[http][parser]") {
  constexpr std::string_view wire = "HTTP/1.1 200 OK\r\n"
                                    "Transfer-Encoding: chunked\r\n"
                                    "Trailer: X-Checksum\r\n"
                                    "\r\n"
                                    "5\r\nhello\r\n"
                                    "6\r\n world\r\n"
                                    "0\r\n"
                                    "X-Checksum: abc\r\n"
                                    "\r\n";
  http::response_parser parser;

  auto const result = feed_bytewise(parser, wire);

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.done());

  auto const response = parser.release();

  REQUIRE(response.body == "hello world");
  REQUIRE(response.trailers.find("X-Checksum") == "abc");
  REQUIRE_FALSE(response.fields.contains("X-Checksum"));
}

TEST_CASE("A body delimited by the end of the stream completes on finish", "[http][parser]") {
  constexpr std::string_view wire = "HTTP/1.1 200 OK\r\n"
                                    "Connection: close\r\n"
                                    "\r\n"
                                    "until the end";
  http::response_parser parser;

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE_FALSE(parser.done());
  REQUIRE_FALSE(parser.finish());
  REQUIRE(parser.done());
  REQUIRE_FALSE(parser.keep_alive());
  REQUIRE(parser.release().body == "until the end");

  // The parser is ready for a new stream.
  auto const next = parser.feed(bytes(simple_response));

  REQUIRE_FALSE(next.error);
  REQUIRE(parser.done());
  REQUIRE(parser.release().body == "hello");
}

TEST_CASE("finish reports a message cut short", "[http][parser]") {
  http::response_parser parser;

  auto const result = parser.feed(bytes(simple_response.substr(0, simple_response.size() - 2)));

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.finish() == http::error::partial_message);
  REQUIRE_FALSE(parser.done());
}

TEST_CASE("finish between two messages is a clean end", "[http][parser]") {
  http::response_parser parser;

  REQUIRE_FALSE(parser.finish());
  REQUIRE_FALSE(parser.done());
}

TEST_CASE("A response to HEAD has no body despite its Content-Length", "[http][parser]") {
  constexpr std::string_view head = "HTTP/1.1 200 OK\r\n"
                                    "Content-Length: 12\r\n"
                                    "\r\n";
  std::string const wire = std::string{head} + std::string{simple_response};
  http::response_parser parser;

  parser.set_request_method(http::verb::head);

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE(result.consumed == head.size());
  REQUIRE(parser.done());
  REQUIRE(parser.release().body.empty());

  // release() forgets the method: the next response is parsed with its body.
  auto const next = parser.feed(bytes(std::string_view{wire}.substr(result.consumed)));

  REQUIRE_FALSE(next.error);
  REQUIRE(parser.release().body == "hello");
}

TEST_CASE("An interim 100 Continue is a message of its own", "[http][parser]") {
  constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string const wire = std::string{interim} + std::string{simple_response};
  http::response_parser parser;

  auto const first = parser.feed(bytes(wire));

  REQUIRE_FALSE(first.error);
  REQUIRE(first.consumed == interim.size());
  REQUIRE(parser.done());
  REQUIRE(parser.release().status == http::status::continue_);

  auto const second = parser.feed(bytes(std::string_view{wire}.substr(first.consumed)));

  REQUIRE_FALSE(second.error);
  REQUIRE(parser.done());
  REQUIRE(parser.release().status == http::status::ok);
}

TEST_CASE("The HEAD method holds across an interim response", "[http][parser]") {
  constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";
  constexpr std::string_view final_response = "HTTP/1.1 200 OK\r\n"
                                              "Content-Length: 12\r\n"
                                              "\r\n";
  std::string const wire = std::string{interim} + std::string{final_response};
  http::response_parser parser;

  parser.set_request_method(http::verb::head);

  auto const first = parser.feed(bytes(wire));

  REQUIRE(parser.release().status == http::status::continue_);

  auto const second = parser.feed(bytes(std::string_view{wire}.substr(first.consumed)));

  REQUIRE_FALSE(second.error);
  REQUIRE(second.consumed == final_response.size());
  REQUIRE(parser.done());

  auto const response = parser.release();

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.body.empty());
}

TEST_CASE("A 204 response has no body", "[http][parser]") {
  constexpr std::string_view wire = "HTTP/1.1 204 No Content\r\n\r\n";
  http::response_parser parser;

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.done());
  REQUIRE(parser.release().status == http::status::no_content);
}

TEST_CASE("Pipelined responses are parsed one at a time", "[http][parser]") {
  std::string const wire = std::string{simple_response} + std::string{simple_response};
  http::response_parser parser;

  auto const first = parser.feed(bytes(wire));

  REQUIRE(first.consumed == simple_response.size());
  REQUIRE(parser.release().body == "hello");

  auto const second = parser.feed(bytes(std::string_view{wire}.substr(first.consumed)));

  REQUIRE(second.consumed == simple_response.size());
  REQUIRE(parser.release().body == "hello");
}

TEST_CASE("A request is parsed", "[http][parser]") {
  constexpr std::string_view wire = "POST /accounts?sort=name HTTP/1.1\r\n"
                                    "Host: example.com\r\n"
                                    "Content-Length: 7\r\n"
                                    "X-Empty:\r\n"
                                    "\r\n"
                                    "{\"a\":1}";
  http::request_parser parser;

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.done());

  auto const request = parser.release();

  REQUIRE(request.method == http::verb::post);
  REQUIRE(request.target == "/accounts?sort=name");
  REQUIRE(request.fields.find("HOST") == "example.com");
  REQUIRE(request.fields.find("x-empty") == "");
  REQUIRE(request.fields.size() == 3);
  REQUIRE(request.body == "{\"a\":1}");
}

TEST_CASE("An HTTP/1.0 request is not kept alive by default", "[http][parser]") {
  http::request_parser parser;

  auto const result = parser.feed(bytes("GET / HTTP/1.0\r\n\r\n"));

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.done());
  REQUIRE_FALSE(parser.keep_alive());
  REQUIRE(parser.release().version == http::version::http_1_0);
}

TEST_CASE("Malformed input is rejected", "[http][parser]") {
  http::response_parser parser;

  auto const result = parser.feed(bytes("HTTP/1.1 OK\r\n\r\n"));

  REQUIRE(result.error == http::error::bad_message);
  REQUIRE_FALSE(parser.error_reason().empty());

  // The parser stays failed.
  REQUIRE(parser.feed(bytes(simple_response)).error == http::error::bad_message);
}

TEST_CASE("A method outside http::verb is rejected", "[http][parser]") {
  http::request_parser parser;

  auto const result = parser.feed(bytes("PURGE /cache HTTP/1.1\r\n\r\n"));

  REQUIRE(result.error == http::error::unsupported_method);
  REQUIRE(parser.error_reason() == "unsupported method");
}

TEST_CASE("A 2xx response to CONNECT opens a tunnel", "[http][parser]") {
  constexpr std::string_view head = "HTTP/1.1 200 Connection established\r\n\r\n";
  std::string const wire = std::string{head} + "\x16\x03\x01";
  http::response_parser parser;

  parser.set_request_method(http::verb::connect);

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE(result.consumed == head.size());
  REQUIRE(parser.done());
  REQUIRE(parser.upgrade());
  REQUIRE(parser.release().body.empty());
  REQUIRE_FALSE(parser.upgrade());
}

TEST_CASE("The size limits are enforced", "[http][parser]") {
  SECTION("header") {
    http::parse_limits limits;

    limits.max_header_size = 16;

    http::response_parser parser{limits};

    REQUIRE(parser.feed(bytes(simple_response)).error == http::error::header_too_large);
  }

  SECTION("field count") {
    http::parse_limits limits;

    limits.max_field_count = 1;

    http::response_parser parser{limits};

    REQUIRE(parser.feed(bytes(simple_response)).error == http::error::header_too_large);
    REQUIRE(parser.error_reason() == "too many fields");
  }

  SECTION("body") {
    http::parse_limits limits;

    limits.max_body_size = 4;

    http::response_parser parser{limits};

    REQUIRE(parser.feed(bytes(simple_response)).error == http::error::body_too_large);
  }
}

TEST_CASE("A protocol switch leaves the next bytes unconsumed", "[http][parser]") {
  constexpr std::string_view head = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Upgrade: websocket\r\n"
                                    "\r\n";
  std::string const wire = std::string{head} + "\x81\x05";
  http::response_parser parser;

  auto const result = parser.feed(bytes(wire));

  REQUIRE_FALSE(result.error);
  REQUIRE(result.consumed == head.size());
  REQUIRE(parser.done());
  REQUIRE(parser.upgrade());
  REQUIRE(parser.release().status == http::status::switching_protocols);
}

TEST_CASE("A parser can be moved between messages", "[http][parser]") {
  http::response_parser parser;

  REQUIRE_FALSE(parser.feed(bytes(simple_response.substr(0, 20))).error);

  auto moved = std::move(parser);

  REQUIRE_FALSE(moved.feed(bytes(simple_response.substr(20))).error);
  REQUIRE(moved.done());
  REQUIRE(moved.release().body == "hello");
}
