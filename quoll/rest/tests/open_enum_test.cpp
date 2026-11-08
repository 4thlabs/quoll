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

#include <quoll/http/local_transport.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// Reflected types need external linkage (see json_codec).
namespace open_enum_test {

enum class state { open, closed };

// No enumerator is 0: a value-initialized one has no name.
enum class level { low = 1, high = 2 };

} // namespace open_enum_test

template <>
struct glz::meta<open_enum_test::state> {
  using enum open_enum_test::state;
  static constexpr auto value = glz::enumerate(open, closed);
};

template <>
struct glz::meta<open_enum_test::level> {
  using enum open_enum_test::level;
  static constexpr auto value = glz::enumerate(low, high);
};

namespace open_enum_test {

namespace rest = quoll::rest;

// After the glz::meta of its enum, as any use of the enum Glaze reads.
struct issue {
  std::string title;
  rest::open_enum<state> status;

  bool operator==(issue const &) const = default;
};

struct graded {
  rest::open_enum<level> grade;
};

static_assert(rest::Parameter<rest::open_enum<state>>);

using quoll::http::status;
using quoll::http::verb;

namespace issues {
using namespace quoll::rest;

// The name of the state the server got, as it got it.
using open_one = endpoint<"issues.open", verb::get, "/open", query<"state", open_enum<state>>,
                          returns<std::string, status::ok, text_codec>>;
using strict_one = endpoint<"issues.strict", verb::get, "/strict", query<"state", state>,
                            returns<std::string, status::ok, text_codec>>;
using in_header = endpoint<"issues.header", verb::get, "/header", header<"X-State", open_enum<state>>,
                           returns<std::string, status::ok, text_codec>>;

using api = rest::api<"/issues", open_one, strict_one, in_header>;
} // namespace issues

struct issues_service {
  std::string operator()(issues::open_one, rest::open_enum<state> given) const {
    return std::string{given.name()} + (given.known() ? " known" : " unknown");
  }

  std::string operator()(issues::strict_one, state given) const {
    return std::string{rest::open_enum<state>{given}.name()};
  }

  std::string operator()(issues::in_header, rest::open_enum<state> given) const {
    return (*this)(issues::open_one{}, std::move(given));
  }
};

} // namespace open_enum_test

namespace {

using namespace open_enum_test;

namespace http = quoll::http;

} // namespace

TEST_CASE("open_enum: a name the enum has reads as its enumerator, another is kept as it came", "[rest][codec]") {
  for (auto const &decoded : {rest::json_codec{}.decode<issue>(R"({"title":"a","status":"merged"})"),
                              rest::lenient_json_codec{}.decode<issue>(R"({"title":"a","status":"merged"})")}) {
    REQUIRE(decoded.value().status.name() == "merged");
    REQUIRE_FALSE(decoded.value().status.known());
    REQUIRE(decoded.value().status != state::open);
  }

  auto const closed = rest::json_codec{}.decode<issue>(R"({"title":"a","status":"closed"})").value().status;

  REQUIRE(closed == state::closed);
  REQUIRE(closed.known() == state::closed);
  REQUIRE(closed.name() == "closed");
}

TEST_CASE("open_enum: written by its name, an unknown one as it came", "[rest][codec]") {
  rest::json_codec const codec;

  REQUIRE(codec.encode(issue{.title = "a", .status = rest::open_enum<state>{state::closed}}) ==
          R"({"title":"a","status":"closed"})");
  REQUIRE(codec.encode(issue{.title = "a", .status = rest::open_enum<state>::named("merged")}) ==
          R"({"title":"a","status":"merged"})");

  // Read, then sent again: unchanged.
  auto const read = codec.decode<issue>(R"({"title":"a","status":"merged"})").value();

  REQUIRE(codec.encode(read) == R"({"title":"a","status":"merged"})");
}

TEST_CASE("open_enum: what is no string is still an error", "[rest][codec]") {
  rest::lenient_json_codec const codec;

  REQUIRE_FALSE(codec.decode<issue>(R"({"title":"a","status":3})").has_value());
  REQUIRE_FALSE(codec.decode<issue>(R"({"title":"a","status":null})").has_value());
}

TEST_CASE("open_enum: the enum itself stays strict, a map key included", "[rest][codec]") {
  rest::json_codec const codec;
  std::map<state, int> const counted{{state::open, 2}, {state::closed, 1}};

  REQUIRE_FALSE(codec.decode<state>(R"("merged")").has_value());
  REQUIRE(codec.encode(counted) == R"({"open":2,"closed":1})");
  REQUIRE(codec.decode<std::map<state, int>>(R"({"open":2,"closed":1})").value() == counted);
}

TEST_CASE("open_enum: as a parameter, any name, and the enum alone still refuses one it does not know",
          "[rest][codec]") {
  REQUIRE(rest::param_traits<rest::open_enum<state>>::from_text("merged")->name() == "merged");
  REQUIRE(rest::param_traits<rest::open_enum<state>>::to_text(rest::open_enum<state>::named("merged")) == "merged");

  http::router router;

  rest::mount<issues::api>(router, issues_service{});

  rest::client<issues::api, http::local_transport> const client{http::local_transport{router}};

  REQUIRE(quoll::test::run(client.call<issues::open_one>(rest::open_enum<state>::named("merged"))).value() ==
          "merged unknown");
  REQUIRE(quoll::test::run(client.call<issues::open_one>(rest::open_enum<state>{state::closed})).value() ==
          "closed known");
  REQUIRE(quoll::test::run(client.call<issues::in_header>(rest::open_enum<state>::named("merged"))).value() ==
          "merged unknown");
  REQUIRE(quoll::test::run(client.call<issues::in_header>(rest::open_enum<state>{state::open})).value() ==
          "open known");

  http::request strict;

  strict.method = http::verb::get;
  strict.target = "/issues/strict?state=merged";

  // Outside REQUIRE: clang-tidy reads a std::move inside it as a use after move.
  auto const refused = quoll::test::run(router.dispatch(std::move(strict))).value().status;

  REQUIRE(refused == http::status::bad_request);
}

TEST_CASE("open_enum: an empty name is kept, and text a string refuses is no parameter", "[rest][codec]") {
  auto const empty = rest::json_codec{}.decode<issue>(R"({"title":"a","status":""})").value().status;

  REQUIRE_FALSE(empty.known());
  REQUIRE(empty.name().empty());
  REQUIRE(rest::json_codec{}.encode(issue{.title = "a", .status = empty}) == R"({"title":"a","status":""})");

  REQUIRE_FALSE(rest::param_traits<rest::open_enum<state>>::from_text("\xff").has_value());
  REQUIRE(rest::param_traits<rest::open_enum<state>>::from_text("")->name().empty());
}

TEST_CASE("open_enum: a value the enum has no name for is not known, and writing it fails", "[rest][codec]") {
  rest::open_enum<level> const unnamed;
  rest::open_enum<level> const cast{static_cast<level>(7)};

  for (auto const &value : {unnamed, cast}) {
    REQUIRE_FALSE(value.known());
    REQUIRE(value.name().empty());
    REQUIRE_THROWS_AS(rest::json_codec{}.encode(graded{.grade = value}), std::invalid_argument);
    REQUIRE_THROWS_AS(rest::param_traits<rest::open_enum<level>>::to_text(value), std::invalid_argument);
  }

  REQUIRE(rest::open_enum<level>{level::high} == level::high);
  REQUIRE(rest::json_codec{}.encode(graded{.grade = rest::open_enum<level>{level::high}}) == R"({"grade":"high"})");
}

TEST_CASE("open_enum: default constructed, the value-initialized enumerator", "[rest][codec]") {
  rest::open_enum<state> const value;

  REQUIRE(value == state::open);
  REQUIRE(value.name() == "open");
}

TEST_CASE("open_enum: a name that JSON escapes reads and writes as text", "[rest][codec]") {
  rest::json_codec const codec;
  auto const escaped = codec.decode<issue>(R"({"title":"a","status":"a\"b\\c\u0001"})").value().status;

  REQUIRE(escaped.name() == "a\"b\\c\x01");
  REQUIRE(codec.encode(issue{.title = "a", .status = escaped}) == R"({"title":"a","status":"a\"b\\c\u0001"})");

  // An escaped spelling of a name the enum has is that enumerator.
  REQUIRE(codec.decode<issue>(R"({"title":"a","status":"op\u0065n"})").value().status == state::open);
}

TEST_CASE("open_enum: in a form, by its name", "[rest][codec]") {
  rest::form_codec const codec;

  REQUIRE(codec.encode(issue{.title = "a", .status = rest::open_enum<state>::named("merged")}) ==
          "title=a&status=merged");

  auto const decoded = codec.decode<issue>("title=a&status=merged").value();

  REQUIRE(decoded.status.name() == "merged");
  REQUIRE(codec.decode<issue>("title=a&status=closed").value().status == state::closed);
}
