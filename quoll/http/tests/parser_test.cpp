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
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

  // Right after a message, too: llhttp alone would report the end of the stream as unsafe there.
  REQUIRE_FALSE(parser.feed(bytes(simple_response)).error);
  REQUIRE(parser.release().body == "hello");
  REQUIRE_FALSE(parser.finish());
  REQUIRE_FALSE(parser.done());

  // And the parser is ready for a new stream.
  REQUIRE_FALSE(parser.feed(bytes(simple_response)).error);
  REQUIRE(parser.release().body == "hello");
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

  // What follows is the new protocol, not an HTTP message.
  auto const next = parser.feed(bytes(std::string_view{wire}.substr(result.consumed)));

  REQUIRE(next.error == http::error::upgraded);
  REQUIRE(next.consumed == 0);
}

TEST_CASE("A parser can be moved between messages", "[http][parser]") {
  http::response_parser parser;

  REQUIRE_FALSE(parser.feed(bytes(simple_response.substr(0, 20))).error);

  auto moved = std::move(parser);

  REQUIRE_FALSE(moved.feed(bytes(simple_response.substr(20))).error);
  REQUIRE(moved.done());
  REQUIRE(moved.release().body == "hello");
}

TEST_CASE("Parser: the head of a message is there before its body", "[http][parser]") {
  http::request_parser parser;
  std::string_view const input = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\npi";

  REQUIRE_FALSE(parser.head_complete());
  REQUIRE_FALSE(parser.feed(bytes(input.substr(0, 20))).error);
  REQUIRE_FALSE(parser.head_complete());
  REQUIRE_FALSE(parser.feed(bytes(input.substr(20))).error);
  REQUIRE(parser.head_complete());
  REQUIRE_FALSE(parser.done());
  REQUIRE(parser.head().target == "/a");
  REQUIRE(parser.head().fields.find("Host") == "x");
  REQUIRE_FALSE(parser.feed(bytes(std::string_view{"ng"})).error);
  REQUIRE(parser.done());
  REQUIRE(parser.release().body == "ping");
  REQUIRE_FALSE(parser.head_complete());
}

TEST_CASE("Parser: a request announcing too large a body is refused at its head", "[http][parser]") {
  http::parse_limits limits;

  limits.max_body_size = 3;

  http::request_parser parser{limits};

  REQUIRE(parser.feed(bytes(std::string_view{"POST /a HTTP/1.1\r\nContent-Length: 4\r\n\r\n"})).error ==
          http::error::body_too_large);
}

TEST_CASE("Parser: a response to HEAD may announce more than the limit, as nothing comes", "[http][parser]") {
  http::parse_limits limits;

  limits.max_body_size = 3;

  http::response_parser parser{limits};

  parser.set_request_method(http::verb::head);

  REQUIRE_FALSE(parser.feed(bytes(std::string_view{"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n"})).error);
  REQUIRE(parser.done());
}

namespace {

// Feeds `wire` in pieces of `size`, taking what comes of the body each time, and hands over what the message is once
// done.
struct pieces {
  std::string body;
  std::size_t most_held = 0;
  std::error_code error;
  bool done = false;
};

template <class Parser>
pieces take_in_pieces(Parser &parser, std::string_view wire, std::size_t size) {
  pieces out;
  std::string piece;

  while (!wire.empty() && !parser.done()) {
    auto const result = parser.feed(bytes(wire.substr(0, size)));

    out.most_held = std::max(out.most_held, parser.head().body.size());
    parser.take_body(piece);
    out.body += piece;
    wire.remove_prefix(result.consumed);

    if (result.error) {
      out.error = result.error;
      return out;
    }
  }

  out.done = parser.done();
  return out;
}

auto const always = [](auto const &) { return true; };

} // namespace

TEST_CASE("Parser: a body in pieces comes as it is fed, past the limit of a whole one", "[http][parser]") {
  auto const size = GENERATE(std::size_t{1}, std::size_t{7}, std::size_t{4096});
  std::string const body(std::size_t{1000}, 'x');
  auto const wire =
      GENERATE_COPY("POST /a HTTP/1.1\r\nContent-Length: 1000\r\n\r\n" + body,
                    "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3e8\r\n" + body + "\r\n0\r\n\r\n");

  http::request_parser parser{{.max_body_size = 10}};

  parser.set_body_streaming(always);

  auto const out = take_in_pieces(parser, wire, size);

  REQUIRE(!out.error);
  REQUIRE(out.done);
  REQUIRE(out.body == body);
  REQUIRE(out.most_held <= size);
  REQUIRE(parser.body_streamed());

  auto const message = parser.release();

  REQUIRE(message.body.empty());
  REQUIRE(message.target == "/a");
  REQUIRE_FALSE(parser.body_streamed());
}

TEST_CASE("Parser: the decision is per message, from its head", "[http][parser]") {
  http::request_parser parser{{.max_body_size = 10}};
  std::vector<std::string> asked;

  parser.set_body_streaming([&](http::request const &head) {
    asked.push_back(head.target);
    return head.fields.contains("X-Asked");
  });

  auto const streamed =
      take_in_pieces(parser, "POST /upload HTTP/1.1\r\nX-Asked: 1\r\nContent-Length: 11\r\n\r\nhello world", 4096);

  REQUIRE(streamed.done);
  REQUIRE(streamed.body == "hello world");
  REQUIRE(parser.body_streamed());
  static_cast<void>(parser.release());

  // On the same connection, a message read whole: what the last one was changes nothing to it.
  REQUIRE(!parser.feed(bytes("POST /small HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi")).error);
  REQUIRE(parser.done());
  REQUIRE_FALSE(parser.body_streamed());
  REQUIRE(parser.release().body == "hi");

  // Not asked for: the limit of a whole body, refused at its head.
  auto const refused = parser.feed(bytes("POST /big HTTP/1.1\r\nContent-Length: 11\r\n\r\n"));

  REQUIRE(refused.error == http::error::body_too_large);
  REQUIRE(asked == std::vector<std::string>{"/upload", "/small", "/big"});
}

TEST_CASE("Parser: the limit of a body in pieces holds, at its head and as it comes", "[http][parser]") {
  http::request_parser announced{{.max_streamed_body_size = 10}};

  announced.set_body_streaming(always);
  REQUIRE(take_in_pieces(announced, "POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", 4096).error ==
          http::error::body_too_large);

  http::request_parser chunked{{.max_streamed_body_size = 10}};

  chunked.set_body_streaming(always);

  // Taken as it comes: the limit counts all of it, and not what is held.
  auto const out =
      take_in_pieces(chunked, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nb\r\nhello world\r\n0\r\n\r\n", 1);

  REQUIRE(out.error == http::error::body_too_large);
  REQUIRE(out.body == "hello worl");

  // A response too: what it announces is known from its head.
  http::response_parser response{{.max_streamed_body_size = 10}};

  response.set_body_streaming(always);
  REQUIRE(take_in_pieces(response, "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n", 4096).error ==
          http::error::body_too_large);

  http::request_parser exact{{.max_streamed_body_size = 10}};

  exact.set_body_streaming(always);
  REQUIRE(take_in_pieces(exact, "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789", 3).body == "0123456789");
}

TEST_CASE("Parser: after a body in pieces, its trailers, and the next message from nothing", "[http][parser]") {
  http::request_parser parser{{.max_streamed_body_size = 12}};

  parser.set_body_streaming(always);

  std::string_view const first =
      "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTrailer: X-Sum\r\n\r\n8\r\n01234567\r\n0\r\nX-Sum: 1\r\n\r\n";
  std::string_view const second = "POST /b HTTP/1.1\r\nContent-Length: 8\r\n\r\nabcdefgh";

  auto const out = take_in_pieces(parser, first, 4096);

  REQUIRE(out.body == "01234567");

  auto const message = parser.release();

  REQUIRE(message.trailers.find("X-Sum") == "1");

  // What it counted went with the first: these two together are past the limit, neither alone.
  auto const next = take_in_pieces(parser, second, 4096);

  REQUIRE(!next.error);
  REQUIRE(next.body == "abcdefgh");
  REQUIRE(parser.release().target == "/b");
}

TEST_CASE("Parser: a response's body in pieces ends with the stream, or is found cut", "[http][parser]") {
  http::response_parser ended;

  ended.set_body_streaming(always);

  auto const out = take_in_pieces(ended, "HTTP/1.1 200 OK\r\n\r\nhello", 2);

  REQUIRE(out.body == "hello");
  REQUIRE(!ended.finish());
  REQUIRE(ended.done());

  http::response_parser cut;

  cut.set_body_streaming(always);

  auto const partial = take_in_pieces(cut, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", 2);

  REQUIRE(partial.body == "hello");
  REQUIRE(cut.finish() == http::error::partial_message);

  // HEAD announces what does not come, and a body in pieces changes nothing to it.
  http::response_parser head;

  head.set_body_streaming(always);
  head.set_request_method(http::verb::head);

  auto const nothing = take_in_pieces(head, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", 4096);

  REQUIRE(nothing.done);
  REQUIRE(nothing.body.empty());
}

TEST_CASE("Parser: a response too large for its limit is refused at its head, unless nothing comes", "[http][parser]") {
  auto const read = [](std::string_view wire, http::verb method = http::verb::get) {
    http::response_parser parser{{.max_body_size = 10}};

    parser.set_request_method(method);

    auto const result = parser.feed(bytes(wire));

    return std::pair{result.error, parser.done()};
  };

  auto const refused = std::pair{std::error_code{http::error::body_too_large}, false};
  auto const whole = std::pair{std::error_code{}, true};

  REQUIRE(read("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n") == refused);
  REQUIRE(read("HTTP/1.1 199 Unknown\r\nContent-Length: 11\r\n\r\n") == refused);
  REQUIRE(read("HTTP/1.1 304 Not Modified\r\nContent-Length: 11\r\n\r\n") == whole);
  REQUIRE(read("HTTP/1.1 204 No Content\r\nContent-Length: 11\r\n\r\n") == whole);
  REQUIRE(read("HTTP/1.1 103 Early Hints\r\nContent-Length: 11\r\n\r\n") == whole);
  REQUIRE(read("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n", http::verb::head) == whole);
}

TEST_CASE("Parser: a body read whole stays whole, whatever takes from it", "[http][parser]") {
  http::request_parser parser;
  std::string piece{"from before"};

  REQUIRE(!parser.feed(bytes("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi")).error);
  parser.take_body(piece);
  REQUIRE(piece.empty());
  REQUIRE(parser.release().body == "hi");
}

TEST_CASE("Parser: the size of a body in pieces is counted past what a size_t holds, under WASM", "[http][parser]") {
  // Only there can a test reach what a size_t counts, in the time of a test.
  if constexpr (sizeof(std::size_t) == 4) {
    constexpr std::uint64_t mebibyte = std::uint64_t{1024} * 1024;
    constexpr auto pieces_allowed = 4097;

    http::response_parser parser{{.max_streamed_body_size = pieces_allowed * mebibyte}};
    std::string const piece(mebibyte, 'x');
    std::string taken;

    parser.set_body_streaming(always);
    REQUIRE(!parser.feed(bytes("HTTP/1.1 200 OK\r\n\r\n")).error);

    for (auto index = 0; index < pieces_allowed; ++index) {
      REQUIRE(!parser.feed(bytes(piece)).error);
      parser.take_body(taken);
    }

    REQUIRE(parser.feed(bytes(piece)).error == http::error::body_too_large);
  } else {
    SUCCEED("a size_t of this platform counts more than a test can send");
  }
}
