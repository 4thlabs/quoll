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

#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

namespace http = quoll::http;

static_assert(http::parse_verb("GET") == http::verb::get);
static_assert(http::parse_verb("get") == std::nullopt);
static_assert(http::to_string(http::verb::delete_) == "DELETE");
static_assert(http::reason_phrase(http::status::not_found) == "Not Found");
static_assert(http::is_informational(http::status::continue_));
static_assert(!http::is_informational(http::status::ok));

} // namespace

TEST_CASE("Field names compare case-insensitively", "[http][fields]") {
  http::fields fields;

  fields.add("Content-Type", "text/plain");

  REQUIRE(fields.find("content-type") == "text/plain");
  REQUIRE(fields.contains("CONTENT-TYPE"));
  REQUIRE_FALSE(fields.find("Content-Length"));
}

TEST_CASE("add keeps repeated fields in order, set replaces them", "[http][fields]") {
  http::fields fields;

  fields.add("Set-Cookie", "a=1");
  fields.add("Accept", "*/*");
  fields.add("set-cookie", "b=2");

  std::vector<std::string> cookies;

  for (auto const &entry : fields) {
    if (entry.name == "Set-Cookie" || entry.name == "set-cookie") {
      cookies.push_back(entry.value);
    }
  }

  REQUIRE(cookies == std::vector<std::string>{"a=1", "b=2"});
  REQUIRE(fields.find("Set-Cookie") == "a=1");

  fields.set("SET-COOKIE", "c=3");

  REQUIRE(fields.size() == 2);
  REQUIRE(fields.find("set-cookie") == "c=3");
}

TEST_CASE("erase removes every field of a name", "[http][fields]") {
  http::fields fields;

  fields.add("Via", "a");
  fields.add("via", "b");
  fields.add("Host", "example.com");

  REQUIRE(fields.erase("VIA") == 2);
  REQUIRE(fields.size() == 1);
  REQUIRE(fields.erase("Via") == 0);
}
