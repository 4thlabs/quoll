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
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/http/websocket_parser.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace http = quoll::http;

using http::websocket_opcode;
using http::websocket_role;
using namespace std::string_view_literals;

constexpr http::websocket_mask key{0x37, 0xfa, 0x21, 0x3d};

// The messages of `bytes`, fed `piece` bytes at a time.
[[nodiscard]] std::vector<http::websocket_message>
messages_of(websocket_role role, std::string_view bytes, std::size_t piece = 1000) {
  http::websocket_parser parser{role};
  std::vector<http::websocket_message> messages;

  for (std::size_t position = 0; position < bytes.size(); position += piece) {
    parser.feed(bytes.substr(position, piece));

    while (auto message = parser.next()) {
      messages.push_back(std::move(*message));
    }
  }

  return messages;
}

// A close frame as a peer may send it, what format_websocket_frame refuses included, masked with `key` if asked.
[[nodiscard]] std::string raw_close(std::string_view payload, bool masked = false) {
  std::string out{"\x88"};

  out.push_back(static_cast<char>((masked ? 0x80U : 0U) | payload.size()));

  if (!masked) {
    out.append(payload);
    return out;
  }

  for (auto const byte : key) {
    out.push_back(static_cast<char>(byte));
  }

  for (std::size_t index = 0; index < payload.size(); ++index) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ key[index % 4]));
  }

  return out;
}

// The error next() fails with, none if it does not.
[[nodiscard]] std::error_code failure_code(http::websocket_parser &parser) {
  try {
    static_cast<void>(parser.next());
  } catch (std::system_error const &failure) {
    return failure.code();
  }

  return {};
}

[[nodiscard]] http::websocket_message text(std::string data) {
  return {.opcode = websocket_opcode::text, .data = std::move(data), .close_code = 0};
}

// The error of the first failure reading `bytes`, none if it reads.
[[nodiscard]] std::error_code failure_of(
    websocket_role role, std::string_view bytes, std::size_t max = http::websocket_parser::default_max_message_size
) {
  http::websocket_parser parser{role, max};

  try {
    parser.feed(bytes);

    while (parser.next()) {
    }
  } catch (std::system_error const &failure) {
    return failure.code();
  }

  return {};
}

} // namespace

TEST_CASE("websocket_parser: the examples of RFC 6455, whole and a byte at a time", "[http][websocket]") {
  auto const piece = GENERATE(std::size_t{1}, std::size_t{3}, std::size_t{1000});

  // A single-frame unmasked text message, to a client.
  REQUIRE(messages_of(websocket_role::client, "\x81\x05Hello"sv, piece) == std::vector{text("Hello")});
  // The same, masked, to a server.
  REQUIRE(
      messages_of(websocket_role::server, "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58"sv, piece) ==
      std::vector{text("Hello")}
  );
  // Fragmented.
  REQUIRE(messages_of(websocket_role::client, "\x01\x03Hel\x80\x02lo"sv, piece) == std::vector{text("Hello")});
  // A ping, and a masked pong.
  REQUIRE(
      messages_of(websocket_role::client, "\x89\x05Hello"sv, piece) ==
      std::vector{http::websocket_message{.opcode = websocket_opcode::ping, .data = "Hello", .close_code = 0}}
  );
  REQUIRE(
      messages_of(websocket_role::server, "\x8a\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58"sv, piece) ==
      std::vector{http::websocket_message{.opcode = websocket_opcode::pong, .data = "Hello", .close_code = 0}}
  );
}

TEST_CASE("websocket_parser: lengths of 16 and 64 bits, binary", "[http][websocket]") {
  std::string const medium(256, 'm');
  std::string const large(std::size_t{65536}, 'l');
  auto const piece = GENERATE(std::size_t{1000}, std::size_t{4096});

  auto const read = messages_of(
      websocket_role::client,
      std::string{"\x82\x7e\x01\x00", 4} + medium + std::string{"\x82\x7f\x00\x00\x00\x00\x00\x01\x00\x00", 10} + large,
      piece
  );

  REQUIRE(read.size() == 2);
  REQUIRE(read[0] == http::websocket_message{.opcode = websocket_opcode::binary, .data = medium, .close_code = 0});
  REQUIRE(read[1] == http::websocket_message{.opcode = websocket_opcode::binary, .data = large, .close_code = 0});
}

TEST_CASE("websocket_parser: control frames between the fragments of a message", "[http][websocket]") {
  auto const bytes = http::format_websocket_frame(websocket_opcode::text, "Hel", false, key) +
                     http::format_websocket_frame(websocket_opcode::ping, "p", true, key) +
                     http::format_websocket_frame(websocket_opcode::continuation, "lo", true, key);

  REQUIRE(
      messages_of(websocket_role::server, bytes, 1) ==
      std::vector{
          http::websocket_message{.opcode = websocket_opcode::ping, .data = "p", .close_code = 0}, text("Hello")
      }
  );
}

TEST_CASE("websocket_parser: a close, its code and reason; nothing after it", "[http][websocket]") {
  auto const bytes = http::format_websocket_close(1000, "bye", key) +
                     http::format_websocket_frame(websocket_opcode::text, "after", true, key);
  http::websocket_parser parser{websocket_role::server};

  parser.feed(bytes);

  REQUIRE(
      parser.next() == http::websocket_message{.opcode = websocket_opcode::close, .data = "bye", .close_code = 1000}
  );
  REQUIRE(parser.closed());
  REQUIRE_FALSE(parser.next());

  // A close with no body: 1005.
  REQUIRE(
      messages_of(websocket_role::client, "\x88\x00"sv) ==
      std::vector{http::websocket_message{.opcode = websocket_opcode::close, .data = "", .close_code = 1005}}
  );
}

TEST_CASE("websocket_parser: text split in the middle of a character, joined", "[http][websocket]") {
  auto const bytes = http::format_websocket_frame(websocket_opcode::text, "\xE2\x82", false) +
                     http::format_websocket_frame(websocket_opcode::continuation, "\xAC", true);

  REQUIRE(messages_of(websocket_role::client, bytes) == std::vector{text("\xE2\x82\xAC")});
}

TEST_CASE("websocket_parser: what breaks the protocol fails with 1002", "[http][websocket]") {
  auto const protocol = std::error_code{http::error::websocket_protocol};

  for (auto const &bytes : std::initializer_list<std::string>{
           // Unmasked to a server.
           std::string{"\x81\x01x"},
           // A reserved bit, a reserved opcode.
           std::string{"\xC1\x81\x00\x00\x00\x00x"sv},
           std::string{"\x83\x81\x00\x00\x00\x00x"sv},
           // A length of 16 bits for 5 bytes.
           std::string{"\x81\xFE\x00\x05\x00\x00\x00\x00hello"sv},
           // A control frame fragmented, or of 126 bytes.
           http::format_websocket_frame(websocket_opcode::text, "x", true, key).replace(0, 1, "\x09"),
           std::string{"\x89\xFE\x00\x7E\x00\x00\x00\x00"sv} + std::string(126, 'p'),
           // A continuation of nothing; a message inside another.
           http::format_websocket_frame(websocket_opcode::continuation, "x", true, key),
           http::format_websocket_frame(websocket_opcode::text, "x", false, key) +
               http::format_websocket_frame(websocket_opcode::text, "y", true, key),
           // A close of one byte, or with a code no one sends.
           std::string{"\x88\x81\x00\x00\x00\x00\x03"sv},
           raw_close("\x03\xED", true),
       }) {
    REQUIRE(failure_of(websocket_role::server, bytes) == protocol);
  }

  // Masked to a client.
  REQUIRE(
      failure_of(websocket_role::client, http::format_websocket_frame(websocket_opcode::text, "x", true, key)) ==
      protocol
  );
}

TEST_CASE(
    "websocket_parser: text that is not UTF-8 fails with 1007, a message too large with 1009", "[http][websocket]"
) {
  REQUIRE(
      failure_of(websocket_role::client, http::format_websocket_frame(websocket_opcode::text, "\xFF")) ==
      http::error::websocket_invalid_text
  );
  REQUIRE(failure_of(websocket_role::client, raw_close("\x03\xE8\xFF")) == http::error::websocket_invalid_text);
  REQUIRE(
      failure_of(websocket_role::client, http::format_websocket_frame(websocket_opcode::binary, "12345"), 4) ==
      http::error::websocket_too_large
  );

  // Fragments that pass the limit together, refused at the second's header, before any of its bytes.
  REQUIRE(
      failure_of(
          websocket_role::client,
          http::format_websocket_frame(websocket_opcode::binary, "123", false) +
              http::format_websocket_frame(websocket_opcode::continuation, "45", true).substr(0, 2),
          4
      ) == http::error::websocket_too_large
  );

  REQUIRE(http::close_code_of(http::error::websocket_protocol) == 1002);
  REQUIRE(http::close_code_of(http::error::websocket_invalid_text) == 1007);
  REQUIRE(http::close_code_of(http::error::websocket_too_large) == 1009);
  REQUIRE(http::close_code_of(std::make_error_code(std::errc::io_error)) == 1011);
}

TEST_CASE(
    "websocket_parser: a failure is for good, and fed again before every message is taken, it refuses",
    "[http][websocket]"
) {
  http::websocket_parser parser{websocket_role::client};

  parser.feed("\x81\x01"
              "a"
              "\x81\x01"
              "b");

  REQUIRE(parser.next() == text("a"));
  REQUIRE_THROWS_AS(
      parser.feed("\x81\x01"
                  "c"),
      std::logic_error
  );
  REQUIRE(parser.next() == text("b"));
  REQUIRE_FALSE(parser.next());

  parser.feed("\x81\x81\x00\x00\x00\x00x"sv);

  REQUIRE_THROWS_AS(parser.next(), std::system_error);
  REQUIRE_THROWS_AS(parser.next(), std::system_error);
  REQUIRE_THROWS_AS(
      parser.feed("\x81\x01"
                  "d"),
      std::system_error
  );
}

TEST_CASE("format_websocket_frame: what it writes reads back, masked or not, of every length", "[http][websocket]") {
  for (auto const size : {std::size_t{0}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}}) {
    std::string const payload(size, 'x');

    REQUIRE(
        messages_of(
            websocket_role::server, http::format_websocket_frame(websocket_opcode::binary, payload, true, key)
        ) == std::vector{http::websocket_message{.opcode = websocket_opcode::binary, .data = payload, .close_code = 0}}
    );
    REQUIRE(
        messages_of(websocket_role::client, http::format_websocket_frame(websocket_opcode::binary, payload)) ==
        std::vector{http::websocket_message{.opcode = websocket_opcode::binary, .data = payload, .close_code = 0}}
    );
  }
}

TEST_CASE("format_websocket_frame: what a peer would refuse is refused", "[http][websocket]") {
  REQUIRE_THROWS_AS(http::format_websocket_frame(websocket_opcode::ping, std::string(126, 'p')), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_frame(websocket_opcode::ping, "p", false), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_close(1005), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_close(999), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_close(1000, "\xFF"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_close(1000, std::string(124, 'r')), std::invalid_argument);
  REQUIRE(http::format_websocket_close(4000, std::string(123, 'r')).size() == 2 + 125);
}

TEST_CASE(
    "websocket_parser: a 64-bit length, in its shortest form, its top bit clear, bounded before its bytes",
    "[http][websocket]"
) {
  auto const protocol = std::error_code{http::error::websocket_protocol};

  // 127 for a length that fits in 16 bits; the top bit set.
  REQUIRE(failure_of(websocket_role::client, "\x82\x7f\x00\x00\x00\x00\x00\x00\xff\xff"sv) == protocol);
  REQUIRE(failure_of(websocket_role::client, "\x82\x7f\x80\x00\x00\x00\x00\x01\x00\x00"sv) == protocol);
  // 2^40 bytes announced: refused at its header, none of them held.
  REQUIRE(
      failure_of(websocket_role::client, "\x82\x7f\x00\x00\x01\x00\x00\x00\x00\x00"sv) ==
      http::error::websocket_too_large
  );
}

TEST_CASE("websocket_parser: text that is not UTF-8 fails at its first fragment, not its last", "[http][websocket]") {
  http::websocket_parser parser{websocket_role::client};

  // A first fragment whose first byte no UTF-8 starts with: refused before the rest comes.
  parser.feed(http::format_websocket_frame(
      websocket_opcode::text,
      "\xFF"
      "abc",
      false
  ));

  REQUIRE(failure_code(parser) == http::error::websocket_invalid_text);

  // A fragment ending in two bytes no character starts with, though cut: refused, not carried over.
  http::websocket_parser cut_wrong{websocket_role::client};

  cut_wrong.feed(http::format_websocket_frame(websocket_opcode::text, "a\xE0\x80", false));

  REQUIRE(failure_code(cut_wrong) == http::error::websocket_invalid_text);

  // A character cut between two fragments is no failure: they read as they come.
  http::websocket_parser cut{websocket_role::client};

  cut.feed(
      http::format_websocket_frame(websocket_opcode::text, "a\xE2\x82", false) + http::format_websocket_frame(
                                                                                     websocket_opcode::continuation,
                                                                                     "\xAC"
                                                                                     "b",
                                                                                     false
                                                                                 )
  );
  REQUIRE_FALSE(cut.next());

  // One never ended at the last is.
  cut.feed(http::format_websocket_frame(websocket_opcode::continuation, "c\xE2", true));
  REQUIRE(failure_code(cut) == http::error::websocket_invalid_text);
}

TEST_CASE("websocket_parser: the close codes a peer may send, at their bounds", "[http][websocket]") {
  auto const close_with = [](std::uint16_t code) {
    std::string payload;

    payload.push_back(static_cast<char>(code >> 8U));
    payload.push_back(static_cast<char>(code & 0xFFU));
    return failure_of(websocket_role::client, raw_close(payload));
  };

  for (std::uint16_t const code : {1000, 1001, 1002, 1003, 1007, 1011, 1012, 1014, 3000, 4999}) {
    REQUIRE_FALSE(close_with(code));
  }

  for (std::uint16_t const code : {999, 1004, 1005, 1006, 1015, 1016, 2999, 5000}) {
    REQUIRE(close_with(code) == http::error::websocket_protocol);
    REQUIRE_THROWS_AS(http::format_websocket_close(code), std::invalid_argument);
  }
}

TEST_CASE("websocket_parser: bytes fed after a close are dropped", "[http][websocket]") {
  http::websocket_parser parser{websocket_role::client};

  parser.feed(http::format_websocket_close(1000));

  REQUIRE(parser.next());
  REQUIRE_FALSE(parser.next());

  parser.feed(http::format_websocket_frame(websocket_opcode::text, "after"));

  REQUIRE_FALSE(parser.next());
  REQUIRE(parser.closed());
}

TEST_CASE(
    "format_websocket_frame: an opcode RFC 6455 does not name, or a close a peer would refuse", "[http][websocket]"
) {
  REQUIRE_THROWS_AS(http::format_websocket_frame(static_cast<websocket_opcode>(3), "x"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_frame(websocket_opcode::close, "\x03"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_frame(websocket_opcode::close, "\x03\xED"), std::invalid_argument);
  REQUIRE_THROWS_AS(http::format_websocket_frame(websocket_opcode::close, "\x03\xE8\xFF"), std::invalid_argument);
  REQUIRE(http::format_websocket_frame(websocket_opcode::close, "") == std::string{"\x88\x00", 2});
}

TEST_CASE(
    "websocket_parser: bytes fed after the close is read, with no next() between, are dropped", "[http][websocket]"
) {
  http::websocket_parser parser{websocket_role::client};

  parser.feed(http::format_websocket_close(1000) + http::format_websocket_frame(websocket_opcode::text, "x"));

  REQUIRE(parser.next());
  REQUIRE(parser.closed());
  REQUIRE_NOTHROW(parser.feed(http::format_websocket_frame(websocket_opcode::text, "more")));
  REQUIRE_FALSE(parser.next());
}

TEST_CASE("websocket_parser: a broken close is a failure, not an end", "[http][websocket]") {
  http::websocket_parser parser{websocket_role::client};

  parser.feed(raw_close("\x03"));

  REQUIRE(failure_code(parser) == http::error::websocket_protocol);
  REQUIRE_FALSE(parser.closed());
}

TEST_CASE("websocket_parser: to a server, masked text cut in the middle of a character, joined", "[http][websocket]") {
  auto const bytes = http::format_websocket_frame(websocket_opcode::text, "a\xE2\x82", false, key) +
                     http::format_websocket_frame(websocket_opcode::ping, "p", true, key) +
                     http::format_websocket_frame(websocket_opcode::continuation, "\xAC", true, key);

  REQUIRE(
      messages_of(websocket_role::server, bytes, 1) ==
      std::vector{
          http::websocket_message{.opcode = websocket_opcode::ping, .data = "p", .close_code = 0}, text("a\xE2\x82\xAC")
      }
  );
}
