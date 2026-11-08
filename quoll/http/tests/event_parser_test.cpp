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
#include <quoll/http/event_parser.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace http = quoll::http;

using namespace std::chrono_literals;
using namespace std::string_view_literals;

// The events of `text`, fed `piece` bytes at a time, as a body comes.
[[nodiscard]] std::vector<http::server_sent_event> events_of(std::string_view text, std::size_t piece) {
  http::event_parser parser;
  std::vector<http::server_sent_event> events;

  for (std::size_t position = 0; position < text.size(); position += piece) {
    parser.feed(text.substr(position, piece));

    while (auto event = parser.next()) {
      events.push_back(std::move(*event));
    }
  }

  return events;
}

[[nodiscard]] http::server_sent_event event(std::string data, std::optional<std::string> id = std::nullopt,
                                            std::string type = "message") {
  return {.type = std::move(type), .data = std::move(data), .id = std::move(id)};
}

} // namespace

TEST_CASE("event_parser: the examples of the standard, whole and a byte at a time", "[http][sse]") {
  auto const piece = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{1000});

  REQUIRE(events_of("data: YHOO\ndata: +2\ndata: 10\n\n", piece) == std::vector{event("YHOO\n+2\n10")});
  REQUIRE(events_of(": test stream\n\ndata: first event\nid: 1\n\ndata:second event\nid\n\ndata:  third event\n\n",
                    piece) == std::vector{event("first event", "1"), event("second event"), event(" third event")});
  REQUIRE(events_of("data\n\ndata\ndata\n\ndata:", piece) == std::vector{event(""), event("\n")});
  REQUIRE(events_of("data:test\n\ndata: test\n\n", piece) == std::vector{event("test"), event("test")});
}

TEST_CASE("event_parser: CRLF, LF and CR end a line, a CRLF split between two pieces too", "[http][sse]") {
  auto const piece = GENERATE(std::size_t{1}, std::size_t{3}, std::size_t{1000});

  REQUIRE(events_of("data: a\r\ndata: b\rdata: c\n\r\ndata: d\r\r", piece) ==
          std::vector{event("a\nb\nc"), event("d")});
}

TEST_CASE("event_parser: a BOM at the start is skipped, even split, and nowhere else", "[http][sse]") {
  auto const piece = GENERATE(std::size_t{1}, std::size_t{2}, std::size_t{1000});

  REQUIRE(events_of("\xEF\xBB\xBF"
                    "data: a\n\n",
                    piece) == std::vector{event("a")});
  // Later, it is part of a field's name, which no field has.
  REQUIRE(events_of("data: a\n\n\xEF\xBB\xBF"
                    "data: b\n\n",
                    piece) == std::vector{event("a")});
  REQUIRE(events_of("\xEF\xBB"
                    "data: a\n\n",
                    piece)
              .empty());
}

TEST_CASE("event_parser: the type, and what resets it", "[http][sse]") {
  REQUIRE(events_of("event: add\ndata: x\n\ndata: y\n\n", 1000) ==
          std::vector{event("x", std::nullopt, "add"), event("y")});
  // A block with no data dispatches nothing, and forgets its type.
  REQUIRE(events_of("event: add\n\ndata: y\n\n", 1000) == std::vector{event("y")});
  REQUIRE(events_of("event\ndata: y\n\n", 1000) == std::vector{event("y")});
}

TEST_CASE("event_parser: the last event ID stays until the stream sets another", "[http][sse]") {
  http::event_parser parser;

  parser.feed("id: 7\ndata: a\n\ndata: b\n\nid: 8\x00x\ndata: c\n\nid: 9\n\n"sv);

  REQUIRE(parser.next() == event("a", "7"));
  REQUIRE(parser.next() == event("b", "7"));
  // An id holding a NUL is ignored.
  REQUIRE(parser.next() == event("c", "7"));
  // A block with no data sets it all the same.
  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.last_event_id() == "9");

  parser.feed("id\ndata: d\n\n");

  REQUIRE(parser.next() == event("d"));
  REQUIRE(parser.last_event_id().empty());
}

TEST_CASE("event_parser: an id counts once its event has ended", "[http][sse]") {
  http::event_parser parser;

  parser.feed("id: 4\ndata: a\n\nid: 5\ndata: b\n");

  REQUIRE(parser.next() == event("a", "4"));
  REQUIRE_FALSE(parser.next());
  // The stream may end here: event 5 has not come, and connecting again has to ask for it.
  REQUIRE(parser.last_event_id() == "4");

  parser.feed("\n");

  REQUIRE(parser.next() == event("b", "5"));
  REQUIRE(parser.last_event_id() == "5");
}

TEST_CASE("event_parser: an id longer than max_id_size is ignored, the stream goes on", "[http][sse]") {
  http::event_parser parser;
  std::string const longest(http::event_parser::max_id_size, 'i');

  parser.feed("id: " + longest + "\ndata: a\n\n");

  REQUIRE(parser.next() == event("a", longest));
  REQUIRE_FALSE(parser.next());

  parser.feed("id: " + longest + "i\ndata: b\n\n");

  REQUIRE(parser.next() == event("b", longest));
  REQUIRE(parser.last_event_id() == longest);
}

TEST_CASE("event_parser: a BOM still to come, then a CR", "[http][sse]") {
  http::event_parser parser;

  parser.feed("");
  REQUIRE_FALSE(parser.next());
  parser.feed("\xEF");
  REQUIRE_FALSE(parser.next());
  parser.feed("\xBB\xBF"
              "data: a\r");
  REQUIRE_FALSE(parser.next());
  parser.feed("\r");
  REQUIRE(parser.next() == event("a"));
}

TEST_CASE("event_parser: retry takes ASCII digits only", "[http][sse]") {
  http::event_parser parser;

  REQUIRE_FALSE(parser.retry());

  for (auto const *const ignored : {"retry: 1.5\n", "retry: \n", "retry: -3\n", "retry: 12a\n", "retry:  12\n"}) {
    parser.feed(ignored);
    REQUIRE_FALSE(parser.next());
    REQUIRE_FALSE(parser.retry());
  }

  parser.feed("retry: 1500\n");

  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.retry() == 1500ms);

  parser.feed("retry: x\n");

  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.retry() == 1500ms);

  // What a peer asks for beyond the most is cut to it, a number too long for any type included.
  for (auto const *const long_one : {"retry: 86400001\n", "retry: 99999999999999999999999\n"}) {
    parser.feed("retry: 1\n");
    REQUIRE_FALSE(parser.next());
    parser.feed(long_one);
    REQUIRE_FALSE(parser.next());
    REQUIRE(parser.retry() == http::event_parser::max_retry);
  }

  parser.feed("retry: 86400000\n");

  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.retry() == 24h);
}

TEST_CASE("event_parser: fed again before every event is taken, it refuses", "[http][sse]") {
  http::event_parser parser;

  parser.feed("data: a\n\ndata: b\n\n");

  REQUIRE(parser.next());
  REQUIRE_THROWS_AS(parser.feed("data: c\n\n"), std::logic_error);
  REQUIRE(parser.next() == event("b"));
  REQUIRE_FALSE(parser.next());

  parser.feed("data: c\n\n");

  REQUIRE(parser.next() == event("c"));
}

TEST_CASE("event_parser: what is not UTF-8 in a comment or an unknown field is dropped with it", "[http][sse]") {
  REQUIRE(events_of(": \xFF\xFF\nfoo: \xFF\nid: a\xFF\nevent: b\xFF\ndata: c\n\n", 1000) ==
          std::vector{event("c", "a\uFFFD", "b\uFFFD")});
}

TEST_CASE("event_parser: unknown fields and comments are ignored, a colon in a value kept", "[http][sse]") {
  REQUIRE(events_of(":comment\nfoo: bar\nDATA: no\ndata: a: b\n\n", 1000) == std::vector{event("a: b")});
}

TEST_CASE("event_parser: what is not UTF-8 reads as U+FFFD, a maximal subpart at a time", "[http][sse]") {
  auto const piece = GENERATE(std::size_t{1}, std::size_t{1000});

  // A lone continuation byte, a truncated sequence, a surrogate, an overlong form, and a valid one after them.
  REQUIRE(events_of("data: a\x80"
                    "b\xE2\x82"
                    "c\xED\xA0\x80"
                    "d\xC0\xAF"
                    "e\xE2\x82\xAC\n\n",
                    piece) == std::vector{event("a�"
                                                "b�"
                                                "c���"
                                                "d��"
                                                "e€")});
  // In a field's name too, which then names no field.
  REQUIRE(events_of("dat\xFF"
                    "a: x\n\ndata: y\n\n",
                    piece) == std::vector{event("y")});
}

TEST_CASE("event_parser: an event the stream does not end is none", "[http][sse]") {
  http::event_parser parser;

  parser.feed("data: a\n\ndata: b\n");

  REQUIRE(parser.next() == event("a"));
  REQUIRE_FALSE(parser.next());
  REQUIRE_FALSE(parser.next());
}

TEST_CASE("event_parser: an event larger than the most it holds fails, and goes on failing", "[http][sse]") {
  auto const throws_too_large = [](http::event_parser &parser) {
    try {
      static_cast<void>(parser.next());
    } catch (std::system_error const &failure) {
      return failure.code() == http::error::event_too_large;
    }

    return false;
  };

  SECTION("a line that never ends") {
    http::event_parser parser{16};

    parser.feed("data: 0123456789");

    REQUIRE_FALSE(parser.next());

    parser.feed("x");

    REQUIRE(throws_too_large(parser));
    REQUIRE(throws_too_large(parser));

    // What comes after is refused, not held.
    try {
      parser.feed("\n\ndata: a\n\n");
      FAIL("fed after the failure");
    } catch (std::system_error const &failure) {
      REQUIRE(failure.code() == http::error::event_too_large);
    }
  }

  SECTION("data over many lines") {
    http::event_parser parser{16};

    parser.feed("data: 0123456789\ndata: 0123456789\n");

    REQUIRE(throws_too_large(parser));
  }

  SECTION("a long line that holds no data, whole or in pieces") {
    std::string const text = ": " + std::string(20, 'x') + "\ndata: a\n\n";
    auto const piece = GENERATE(std::size_t{1}, std::size_t{1000});
    http::event_parser parser{16};
    bool failed = false;

    for (std::size_t position = 0; position < text.size() && !failed; position += piece) {
      parser.feed(std::string_view{text}.substr(position, piece));
      failed = throws_too_large(parser);
    }

    REQUIRE(failed);
  }

  SECTION("the type counts, not the one an event line replaces") {
    http::event_parser parser{16};

    parser.feed("event: 0123456\nevent: 0123456\ndata: x\n\n");

    REQUIRE(parser.next() == event("x", std::nullopt, "0123456"));
    REQUIRE_FALSE(parser.next());

    parser.feed("event: 01234567\ndata: 012345678\n\n");

    REQUIRE(throws_too_large(parser));
  }

  SECTION("what is not UTF-8 counts as read, U+FFFD each") {
    http::event_parser parser{16};

    // 6 bytes, 18 once read.
    parser.feed("data: \xFF\xFF\xFF\xFF\xFF\xFF\n\n");

    REQUIRE(throws_too_large(parser));
  }

  SECTION("an event at the limit, and many of them") {
    http::event_parser parser{16};
    std::string text;

    for (int index = 0; index < 100; ++index) {
      text += "data: 0123456789\n\n";
    }

    parser.feed(text);

    for (int index = 0; index < 100; ++index) {
      REQUIRE(parser.next() == event("0123456789"));
    }

    REQUIRE_FALSE(parser.next());
  }
}
