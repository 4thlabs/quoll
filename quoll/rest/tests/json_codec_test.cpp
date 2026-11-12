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

#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/text_codec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Reflected types need external linkage: Clang cannot reflect the fields of a type in an anonymous namespace.
namespace json_codec_test {

enum class card_status { active, frozen };

struct address {
  std::string city;
};

struct user {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> email;
  std::vector<address> addresses;
  card_status status = card_status::active;
};

// The shape of a third-party API: JSON names that are not the C++ ones.
struct repository {
  std::string full_name;
  std::int64_t stars = 0;
};

} // namespace json_codec_test

template <>
struct glz::meta<json_codec_test::card_status> {
  using enum json_codec_test::card_status;
  static constexpr auto value = glz::enumerate(active, frozen);
};

template <>
struct glz::meta<json_codec_test::repository> {
  using T = json_codec_test::repository;
  static constexpr auto value = glz::object("full_name", &T::full_name, "stargazers_count", &T::stars);
};

namespace {

using json_codec_test::card_status;
using json_codec_test::repository;
using json_codec_test::user;
using quoll::rest::json_codec;
using quoll::rest::lenient_json_codec;

constexpr json_codec json;
constexpr lenient_json_codec lenient;

} // namespace

TEST_CASE("json_codec: a value goes out and comes back", "[rest][json]") {
  user const sent{
      .id = 9'007'199'254'740'993,
      .name = "Ada",
      .email = "ada@example.com",
      .addresses = {{"Paris"}, {"London"}},
      .status = card_status::frozen
  };
  auto const body = json.encode(sent);
  auto const received = json.decode<user>(body);

  REQUIRE(received.has_value());
  // Beyond 2^53: exact, unlike a JavaScript number.
  REQUIRE(received->id == 9'007'199'254'740'993);
  REQUIRE(received->name == "Ada");
  REQUIRE(received->email == "ada@example.com");
  REQUIRE(received->addresses.size() == 2);
  REQUIRE(received->addresses[1].city == "London");
  REQUIRE(received->status == card_status::frozen);
  REQUIRE(body.find("\"frozen\"") != std::string::npos);
}

TEST_CASE("json_codec: an empty optional is not written, and may be missing", "[rest][json]") {
  user sent;

  sent.id = 1;
  sent.name = "Ada";

  auto const body = json.encode(sent);

  REQUIRE(body.find("email") == std::string::npos);

  auto const received = json.decode<user>(R"({"id":1,"name":"Ada","addresses":[],"status":"active"})");

  REQUIRE(received.has_value());
  REQUIRE_FALSE(received->email.has_value());
}

TEST_CASE("json_codec: a missing required field is an error naming it", "[rest][json]") {
  auto const received = json.decode<user>(R"({"name":"Ada","addresses":[],"status":"active"})");

  REQUIRE_FALSE(received.has_value());
  REQUIRE(received.error().message.find("missing_key") != std::string::npos);
  REQUIRE(received.error().message.find("id") != std::string::npos);
}

TEST_CASE("json_codec: a value of the wrong type is an error with its position", "[rest][json]") {
  auto const received = json.decode<user>(R"({"id":"one","name":"Ada","addresses":[],"status":"active"})");

  REQUIRE_FALSE(received.has_value());
  REQUIRE(received.error().message.find("1:7") != std::string::npos);
}

TEST_CASE("json_codec: an unknown enum name is an error", "[rest][json]") {
  auto const received = json.decode<user>(R"({"id":1,"name":"Ada","addresses":[],"status":"lost"})");

  REQUIRE_FALSE(received.has_value());
}

TEST_CASE("json_codec: unknown keys are errors, the lenient codec skips them", "[rest][json]") {
  std::string_view const body = R"({"full_name":"4thlabs/quoll","stargazers_count":3,"private":false})";

  REQUIRE_FALSE(json.decode<repository>(body).has_value());

  auto const received = lenient.decode<repository>(body);

  REQUIRE(received.has_value());
  REQUIRE(received->full_name == "4thlabs/quoll");
  REQUIRE(received->stars == 3);
}

TEST_CASE("json_codec: glz::meta renames fields both ways", "[rest][json]") {
  auto const body = json.encode(repository{.full_name = "4thlabs/quoll", .stars = 3});

  REQUIRE(body == R"({"full_name":"4thlabs/quoll","stargazers_count":3})");
}

TEST_CASE("json_codec: a body that is part of a larger buffer", "[rest][json]") {
  // Not null-terminated: the view stops before the rest of the buffer.
  std::string const buffer = R"({"full_name":"a","stargazers_count":1}{"trailing":true})";
  auto const received = json.decode<repository>(std::string_view{buffer}.substr(0, buffer.find('}') + 1));

  REQUIRE(received.has_value());
  REQUIRE(received->stars == 1);
}

TEST_CASE("json_codec: anything but whitespace after the value is an error", "[rest][json]") {
  REQUIRE(json.decode<repository>(R"({"full_name":"a","stargazers_count":1} )"
                                  "\n")
              .has_value());
  REQUIRE_FALSE(json.decode<repository>(R"({"full_name":"a","stargazers_count":1}xyz)").has_value());
  REQUIRE_FALSE(
      json.decode<repository>(R"({"full_name":"a","stargazers_count":1}{"full_name":"b","stargazers_count":2})")
          .has_value()
  );
  REQUIRE_FALSE(lenient.decode<repository>(R"({"full_name":"a","stargazers_count":1},)").has_value());
}

TEST_CASE("json_codec: a value Glaze cannot write throws", "[rest][json]") {
  user sent;

  sent.status = static_cast<card_status>(7);

  REQUIRE_THROWS_AS(json.encode(sent), std::invalid_argument);
}

TEST_CASE("text_codec: text goes out and comes back as it is", "[rest]") {
  constexpr quoll::rest::text_codec text;

  REQUIRE(text.encode(std::string{"hello"}) == "hello");
  REQUIRE(text.decode<std::string>("hello") == "hello");
}

TEST_CASE("json_codec: what it writes it reads back, control characters included", "[rest][json]") {
  quoll::rest::json_codec const json;

  // Short and long: Glaze writes a long one another way.
  for (std::string const &name :
       {std::string{"a\x01z"}, std::string(81, '\x02'), std::string(81, 'x') + '\x1f', std::string{"\t\n\"\\"},
        std::string{"caf\xC3\xA9 \xF0\x9F\x90\xA8"}}) {
    json_codec_test::address const place{.city = name};
    auto const written = json.encode(place);
    auto const read = json.decode<json_codec_test::address>(written);

    REQUIRE(read.has_value());
    REQUIRE(read->city == name);
  }
}

TEST_CASE("json_codec: text that is not UTF-8 is refused rather than written", "[rest][json]") {
  quoll::rest::json_codec const json;

  for (std::string const &name :
       {std::string{"a\xffz"}, std::string{"\xC0\x80"}, std::string{"\xED\xA0\x80"}, std::string{"\xF4\x90\x80\x80"},
        std::string{"\xE2\x82"}, std::string{"\xF0\x80\x80\x80"}, std::string{"\x80"}}) {
    REQUIRE_THROWS_AS((void)json.encode(json_codec_test::address{.city = name}), std::invalid_argument);
  }
}

TEST_CASE("json_codec: what it says of a body is text, whatever the body holds", "[rest][json]") {
  quoll::rest::json_codec const json;
  auto const refused = json.decode<json_codec_test::address>(std::string{"{\"city\":\xff\x01\0}", 12});

  REQUIRE_FALSE(refused.has_value());

  for (auto const character : refused.error().message) {
    auto const byte = static_cast<unsigned char>(character);

    REQUIRE((character == '\n' || (byte >= 0x20 && byte < 0x7F)));
  }
}
