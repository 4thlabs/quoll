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

#include <quoll/rest/param_traits.hpp>
#include <quoll/rest/parameter.hpp>

#include <catch2/catch_test_macros.hpp>

#if __has_include(<stdfloat>)
#include <stdfloat>
#endif

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

// Reflected types need external linkage (see json_codec).
namespace param_traits_test {

enum class sort_order { ascending, descending };

} // namespace param_traits_test

template <>
struct glz::meta<param_traits_test::sort_order> {
  using enum param_traits_test::sort_order;
  static constexpr auto value = glz::enumerate(ascending, descending);
};

namespace {

using param_traits_test::sort_order;
using quoll::rest::param_traits;

static_assert(quoll::rest::Parameter<sort_order>);
// Integral, but not numbers std::from_chars reads: no traits, a clean constraint failure.
static_assert(!quoll::rest::Parameter<char8_t>);
static_assert(!quoll::rest::Parameter<wchar_t>);

} // namespace

TEST_CASE("param_traits: integers in decimal, the whole text", "[rest][param]") {
  REQUIRE(param_traits<int>::from_text("42") == 42);
  REQUIRE(param_traits<int>::from_text("-7") == -7);
  REQUIRE_FALSE(param_traits<int>::from_text(""));
  REQUIRE_FALSE(param_traits<int>::from_text("12a"));
  REQUIRE_FALSE(param_traits<int>::from_text("+1"));
  REQUIRE_FALSE(param_traits<int>::from_text(" 1"));
  REQUIRE_FALSE(param_traits<int>::from_text("99999999999"));
  REQUIRE_FALSE(param_traits<unsigned>::from_text("-1"));

  constexpr auto largest = std::numeric_limits<std::int64_t>::max();

  REQUIRE(param_traits<std::int64_t>::to_text(largest) == "9223372036854775807");
  REQUIRE(param_traits<std::int64_t>::from_text(param_traits<std::int64_t>::to_text(largest)) == largest);
}

TEST_CASE("param_traits: floating-point numbers in their shortest form", "[rest][param]") {
  REQUIRE(param_traits<double>::from_text("1.5") == 1.5);
  REQUIRE(param_traits<double>::to_text(0.1) == "0.1");
  REQUIRE(param_traits<double>::from_text(param_traits<double>::to_text(1.0 / 3.0)) == 1.0 / 3.0);
  REQUIRE_FALSE(param_traits<double>::from_text("one"));
  REQUIRE_FALSE(param_traits<double>::from_text("-"));
  // from_chars reads these, but no parameter means them.
  REQUIRE_FALSE(param_traits<double>::from_text("nan"));
  REQUIRE_FALSE(param_traits<double>::from_text("inf"));
  REQUIRE_FALSE(param_traits<double>::from_text("-Infinity"));
  REQUIRE_FALSE(param_traits<double>::from_text("1e999"));
}

TEST_CASE("param_traits: booleans are true or false", "[rest][param]") {
  REQUIRE(param_traits<bool>::from_text("true") == true);
  REQUIRE(param_traits<bool>::from_text("false") == false);
  REQUIRE_FALSE(param_traits<bool>::from_text("1"));
  REQUIRE_FALSE(param_traits<bool>::from_text("True"));
  REQUIRE(param_traits<bool>::to_text(true) == "true");
}

TEST_CASE("param_traits: strings as they are", "[rest][param]") {
  REQUIRE(param_traits<std::string>::from_text("a b") == "a b");
  REQUIRE(param_traits<std::string>::to_text("a b") == "a b");
}

TEST_CASE("param_traits: enums by name", "[rest][param]") {
  REQUIRE(param_traits<sort_order>::to_text(sort_order::descending) == "descending");
  REQUIRE(param_traits<sort_order>::from_text("ascending") == sort_order::ascending);
  REQUIRE_FALSE(param_traits<sort_order>::from_text("sideways"));
  REQUIRE_FALSE(param_traits<sort_order>::from_text("ascending\""));
  REQUIRE_FALSE(param_traits<sort_order>::from_text("\\u0061scending"));
  REQUIRE_THROWS_AS(param_traits<sort_order>::to_text(static_cast<sort_order>(7)), std::invalid_argument);
}

// libc++ has no std::from_chars for a long double.
#ifdef __GLIBCXX__
TEST_CASE("param_traits: a floating-point number that fits no double reads back", "[rest]") {
  auto const third = 1.0L / 3.0L;
  auto const text = quoll::rest::param_traits<long double>::to_text(third);

  REQUIRE(text.find('\0') == std::string::npos);
  REQUIRE(quoll::rest::param_traits<long double>::from_text(text) == third);

#ifdef __STDCPP_FLOAT128_T__
  auto const quad = std::float128_t{1} / std::float128_t{3};
  auto const quad_text = quoll::rest::param_traits<std::float128_t>::to_text(quad);

  REQUIRE(quad_text.find('\0') == std::string::npos);
  REQUIRE(quoll::rest::param_traits<std::float128_t>::from_text(quad_text) == quad);
#endif
}
#endif

TEST_CASE("param_traits: a number that is not finite has no text", "[rest]") {
  REQUIRE_THROWS_AS(
      quoll::rest::param_traits<double>::to_text(std::numeric_limits<double>::infinity()), std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      quoll::rest::param_traits<float>::to_text(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument
  );
}
