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

#include <quoll/async/task.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reflected types need external linkage (see json_codec).
namespace codecs_test {

namespace rest = quoll::rest;

using quoll::async::task;
using quoll::http::status;
using quoll::http::verb;

struct login {
  std::string username;
  std::optional<int> age;
  std::vector<std::string> tag;

  bool operator==(login const &) const = default;
};

namespace uploads {
using namespace quoll::rest;

// A form in, bytes out: the server answers the bytes of the name it got, reversed.
using reversed = endpoint<"uploads.reversed", verb::post, "/reversed", body<login, form_codec>,
                          returns<std::vector<std::byte>, status::ok, bytes_codec>>;

using api = rest::api<"/uploads", reversed>;
} // namespace uploads

struct uploads_service {
  std::vector<std::byte> operator()(uploads::reversed, login form) const {
    std::vector<std::byte> bytes;

    for (auto const character : form.username) {
      bytes.push_back(static_cast<std::byte>(character));
    }

    std::ranges::reverse(bytes);
    return bytes;
  }
};

} // namespace codecs_test

namespace {

using namespace codecs_test;

namespace http = quoll::http;

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;

  for (auto const character : text) {
    bytes.push_back(static_cast<std::byte>(character));
  }

  return bytes;
}

} // namespace

TEST_CASE("bytes_codec: bodies as they are, NUL included", "[rest][codec]") {
  rest::bytes_codec const codec;
  std::string_view const raw{"a\0b\xff", 4};

  REQUIRE(codec.content_type == "application/octet-stream");
  REQUIRE(codec.encode(bytes_of(raw)) == raw);
  REQUIRE(codec.encode(std::string{raw}) == raw);
  REQUIRE(codec.decode<std::vector<std::byte>>(raw).value() == bytes_of(raw));
  REQUIRE(codec.decode<std::string>(raw).value() == raw);
  REQUIRE(codec.decode<std::vector<std::byte>>("").value().empty());
  REQUIRE(codec.encode(std::vector<std::byte>{}).empty());
}

TEST_CASE("form_codec: fields by name, encoded as a query is, and decoded back", "[rest][codec]") {
  rest::form_codec const codec;
  login const sent{.username = "Ada Lovelace & co", .age = 36, .tag = {"a", "b=c"}};
  auto const body = codec.encode(sent);

  REQUIRE(codec.content_type == "application/x-www-form-urlencoded");
  REQUIRE(body == "username=Ada%20Lovelace%20%26%20co&age=36&tag=a&tag=b%3Dc");
  REQUIRE(codec.decode<login>(body).value() == sent);

  // As a browser sends it: '+' for a space; an optional left out, a field it does not know ignored.
  REQUIRE(codec.decode<login>("username=Ada+Lovelace&other=1").value() ==
          login{.username = "Ada Lovelace", .age = std::nullopt, .tag = {}});
}

TEST_CASE("form_codec: a form that does not decode is a decode_error", "[rest][codec]") {
  rest::form_codec const codec;

  REQUIRE_FALSE(codec.decode<login>("age=36").has_value());
  REQUIRE_FALSE(codec.decode<login>("username=%zz").has_value());
  REQUIRE_FALSE(codec.decode<login>("username=Ada&age=old").has_value());
  REQUIRE(codec.decode<login>("username=%zz").error().message == "a form that is not URL-encoded");
}

TEST_CASE("form_codec and bytes_codec: a form sent, bytes answered, through a server", "[rest][codec]") {
  http::router router;

  rest::mount<uploads::api>(router, uploads_service{});

  rest::client<uploads::api, http::local_transport> const client{http::local_transport{router}};
  auto const answer = quoll::test::run(client.call<uploads::reversed>({.username = "Ada"}));

  REQUIRE(answer.value() == bytes_of("adA"));
}
