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

#include <quoll/rest/detail/aggregate_fields.hpp>
#include <quoll/rest/detail/field_names.hpp>
#include <quoll/rest/detail/fields_by_name.hpp>
#include <quoll/rest/detail/fields_of.hpp>
#include <quoll/rest/form_codec.hpp>
#include <quoll/rest/names.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aggregate_fields_test {

enum class tint { red, green };

// A type a field of which is made explicitly: counted as one field all the same.
struct made_explicitly {
  made_explicitly() = default;

  explicit made_explicitly(tint /*given*/) {}
};

struct point {
  int x = 0;
  int y = 0;
};

struct mixed {
  std::optional<std::string> contains;
  std::optional<std::int32_t> page_2;
  std::vector<std::string> tags;
  made_explicitly made;
  point where;
  tint color = tint::red;
  std::map<std::string, int> counts;
  std::variant<int, std::string> either;
  std::string_view view;
  bool _flag = false;
};

struct empty {};

// A template, whose arguments GCC writes in what it says of a field, brackets included.
template <class T>
struct tagged {
  int alpha = 0;
  int beta = 0;
};

struct wide {
  int f0 = 0;
  int f1 = 1;
  int f2 = 2;
  int f3 = 3;
  int f4 = 4;
  int f5 = 5;
  int f6 = 6;
  int f7 = 7;
  int f8 = 8;
  int f9 = 9;
  int f10 = 10;
  int f11 = 11;
  int f12 = 12;
  int f13 = 13;
  int f14 = 14;
  int f15 = 15;
  int f16 = 16;
  int f17 = 17;
  int f18 = 18;
  int f19 = 19;
  int f20 = 20;
  int f21 = 21;
  int f22 = 22;
  int f23 = 23;
  int f24 = 24;
  int f25 = 25;
  int f26 = 26;
  int f27 = 27;
  int f28 = 28;
  int f29 = 29;
  int f30 = 30;
  int f31 = 31;
  int f32 = 32;
  int f33 = 33;
  int f34 = 34;
  int f35 = 35;
  int f36 = 36;
  int f37 = 37;
  int f38 = 38;
  int f39 = 39;
  int f40 = 40;
  int f41 = 41;
  int f42 = 42;
  int f43 = 43;
  int f44 = 44;
  int f45 = 45;
  int f46 = 46;
  int f47 = 47;
  int f48 = 48;
  int f49 = 49;
  int f50 = 50;
  int f51 = 51;
  int f52 = 52;
  int f53 = 53;
  int f54 = 54;
  int f55 = 55;
  int f56 = 56;
  int f57 = 57;
  int f58 = 58;
  int f59 = 59;
  int f60 = 60;
  int f61 = 61;
  int f62 = 62;
  int f63 = 63;
};

// Two of its fields go by names that are not C++ names; the third keeps its own.
struct charge {
  std::string order_id;
  std::int64_t amount = 0;
  std::optional<std::string> api_version;

  using rest_names = quoll::rest::names<
      quoll::rest::name<"metadata[order_id]", &charge::order_id>,
      quoll::rest::name<"api-version", &charge::api_version>>;

  bool operator==(charge const &) const = default;
};

// An empty field sharing its address with the next: renaming the next renames it alone.
struct empty_tag {};

struct tagged_field {
  [[no_unique_address]] empty_tag tag;
  int x = 0;

  using rest_names = quoll::rest::names<quoll::rest::name<"renamed", &tagged_field::x>>;
};

} // namespace aggregate_fields_test

using namespace aggregate_fields_test;
using quoll::rest::detail::aggregate_field_count;
using quoll::rest::detail::aggregate_field_names;
using quoll::rest::detail::FieldsByName;

static_assert(aggregate_field_count<mixed>() == 10);
static_assert(aggregate_field_count<point>() == 2);
static_assert(aggregate_field_count<empty>() == 0);
static_assert(aggregate_field_count<wide>() == 64);
static_assert(FieldsByName<mixed> && FieldsByName<wide>);
static_assert(!FieldsByName<std::vector<int>> && !FieldsByName<std::array<int, 2>>);

// Read at compile time: the names are constants.
static_assert(aggregate_field_names<point>[1] == "y");

TEST_CASE("aggregate fields: each named as declared, whatever its type", "[rest][reflect]") {
  constexpr std::array<std::string_view, 10> expected{"contains", "page_2", "tags",   "made", "where",
                                                      "color",    "counts", "either", "view", "_flag"};

  CHECK(aggregate_field_names<mixed> == expected);
  CHECK(aggregate_field_names<wide>[0] == "f0");
  CHECK(aggregate_field_names<wide>[63] == "f63");
  CHECK(aggregate_field_names<empty>.empty());
  CHECK(aggregate_field_names<tagged<int[2]>> == std::array<std::string_view, 2>{"alpha", "beta"});
  CHECK(aggregate_field_names<tagged<std::array<int, 3>>>[1] == "beta");
}

TEST_CASE("aggregate fields: visited with their names, to fill or to read", "[rest][reflect]") {
  point filled;

  quoll::rest::detail::for_each_field(filled, [](std::string_view name, int &field) { field = name == "x" ? 3 : 4; });
  CHECK(filled.x == 3);
  CHECK(filled.y == 4);

  point const read{.x = 1, .y = 2};
  std::string seen;

  quoll::rest::detail::for_each_field(read, [&seen](std::string_view name, int const &field) {
    seen += std::string{name} + "=" + std::to_string(field) + ";";
  });
  CHECK(seen == "x=1;y=2;");
}

TEST_CASE("aggregate fields: rest_names rename the fields they name, and only those", "[rest][reflect]") {
  using quoll::rest::detail::field_names;

  CHECK(field_names<charge> == std::array<std::string_view, 3>{"metadata[order_id]", "amount", "api-version"});
  CHECK(field_names<point> == aggregate_field_names<point>);
  CHECK(field_names<tagged_field> == std::array<std::string_view, 2>{"tag", "renamed"});

  // A form goes by those names, both ways.
  quoll::rest::form_codec const form;
  charge const sent{.order_id = "A7", .amount = 1200, .api_version = "2024-06"};
  auto const encoded = form.encode(sent);

  CHECK(encoded.find("amount=1200") != std::string::npos);
  CHECK(encoded.find("api-version=2024-06") != std::string::npos);
  CAPTURE(encoded);
  CHECK(encoded.starts_with("metadata%5Border_id%5D=A7&"));
  CHECK(form.decode<charge>(encoded) == sent);
}
