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

#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/event_parser.hpp>
#include <quoll/http/format_event.hpp>
#include <quoll/http/server_sent_event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace http = quoll::http;

using namespace std::chrono_literals;
using namespace std::string_view_literals;

// What a reader makes of `text`: the one event it holds.
[[nodiscard]] std::optional<http::server_sent_event> read_back(std::string const &text) {
  http::event_parser parser;

  parser.feed(text);

  auto event = parser.next();

  return parser.next() ? std::nullopt : event;
}

} // namespace

TEST_CASE("format_event: the fields of an event, a line of data each", "[http][sse]") {
  REQUIRE(http::format_event({.data = "hello"}) == "data: hello\n\n");
  REQUIRE(http::format_event({.type = "add", .data = "a\nb", .id = "7"}) == "event: add\nid: 7\ndata: a\ndata: b\n\n");
  REQUIRE(http::format_event({.data = ""}) == "data: \n\n");
  REQUIRE(http::format_event({.type = "", .data = "x", .id = ""}) == "id: \ndata: x\n\n");
}

TEST_CASE("format_event: what it writes reads back the same", "[http][sse]") {
  for (http::server_sent_event const &event : {
           http::server_sent_event{.data = "hello"},
           http::server_sent_event{.type = "add", .data = "a\n\nb\n", .id = "7"},
           http::server_sent_event{.data = ""},
           http::server_sent_event{.data = " leading space, a: colon"},
           http::server_sent_event{.type = " spaced", .data = ":not a comment", .id = " 8"},
           http::server_sent_event{.data = "\xE2\x82\xAC and \U0001F600"},
       }) {
    REQUIRE(read_back(http::format_event(event)) == event);
  }
}

TEST_CASE("format_event: CRLF and CR in the data read back as LF", "[http][sse]") {
  auto const read = read_back(http::format_event({.data = "a\r\nb\rc\n"}));

  REQUIRE(read);
  REQUIRE(read->data == "a\nb\nc\n");
}

TEST_CASE("format_event: what a reader would read otherwise is refused", "[http][sse]") {
  REQUIRE_THROWS_AS(http::format_event({.type = "a\nb", .data = "x"}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_event({.type = "a\rb", .data = "x"}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_event({.data = "x", .id = "1\n2"}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      http::format_event(
          {.data = "x",
           .id = std::string{"1\0"
                             "2"sv}}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(http::format_event({.data = "\xFF"}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_event({.type = "\xC0\xAF", .data = "x"}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_event({.data = "x", .id = "\xED\xA0\x80"}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      http::format_event({.data = "x", .id = std::string(http::max_event_id_size + 1, 'i')}), std::invalid_argument
  );
  REQUIRE(read_back(http::format_event({.data = "x", .id = std::string(http::max_event_id_size, 'i')})));
}

TEST_CASE("format_retry and format_comment: read, and dispatching nothing", "[http][sse]") {
  REQUIRE(http::format_retry(1500ms) == "retry: 1500\n");
  REQUIRE(http::format_comment() == ":\n");
  REQUIRE(http::format_comment("keep alive") == ": keep alive\n");
  REQUIRE_THROWS_AS(http::format_retry(-1ms), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_comment("a\nb"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_comment("\xFF"), std::invalid_argument);

  http::event_parser parser;

  parser.feed(http::format_retry(1500ms) + http::format_comment("ping") + http::format_event({.data = "x"}));

  REQUIRE(parser.next() == http::server_sent_event{.data = "x"});
  REQUIRE(parser.retry() == 1500ms);
  REQUIRE_FALSE(parser.next());

  // A longer time than a reader takes reads as the most it takes.
  parser.feed(http::format_retry(48h));

  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.retry() == http::event_parser::max_retry);
}

TEST_CASE("utf8: the bounds of the Encoding Standard, checked and repaired", "[http][sse]") {
  using quoll::http::detail::append_repaired_utf8;
  using quoll::http::detail::is_utf8;

  auto const repaired = [](std::string_view text) {
    std::string out;

    append_repaired_utf8(out, text);
    return out;
  };

  // The last code point, and one past it; an overlong four-byte form; leads that never start one.
  REQUIRE(is_utf8("\xF4\x8F\xBF\xBF"));
  REQUIRE_FALSE(is_utf8("\xF4\x90\x80\x80"));
  REQUIRE_FALSE(is_utf8("\xF0\x8F\xBF\xBF"));
  REQUIRE(is_utf8("\xF0\x90\x80\x80"));

  // Past eight bytes of ASCII, read eight at a time: what is wrong is found wherever it is, and what is right after.
  for (std::size_t at = 0; at <= 20; ++at) {
    std::string text(20, 'a');

    text.insert(at, "\xC3\xA9");
    REQUIRE(is_utf8(text));
    text[at] = '\xFF';
    REQUIRE_FALSE(is_utf8(text));
  }

  for (auto const *const lead : {"\xC0", "\xC1", "\xF5", "\xFF"}) {
    REQUIRE_FALSE(is_utf8(lead));
    REQUIRE(repaired(lead) == "\uFFFD");
  }

  // A maximal subpart a U+FFFD: F4 then 90 is none, so each byte is one.
  REQUIRE(repaired("\xF4\x90\x80\x80") == "\uFFFD\uFFFD\uFFFD\uFFFD");
  // Cut at the end of the text: one for what came.
  REQUIRE(repaired("a\xF0\x9F\x98") == "a\uFFFD");
  REQUIRE_FALSE(is_utf8("a\xF0\x9F\x98"));
  REQUIRE(repaired("\xE2\x82\xAC") == "\xE2\x82\xAC");
}
