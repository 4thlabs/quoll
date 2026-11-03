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

#include <quoll/http/detail/percent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace {

using quoll::http::detail::percent_decode;
using quoll::http::detail::percent_encode;

} // namespace

TEST_CASE("percent_encode: everything but the unreserved characters and those kept", "[http][percent]") {
  REQUIRE(percent_encode("az-AZ_09.~") == "az-AZ_09.~");
  REQUIRE(percent_encode("a b/c?d&e=f+g%") == "a%20b%2Fc%3Fd%26e%3Df%2Bg%25");
  REQUIRE(percent_encode("dir/a b", "/") == "dir/a%20b");
  REQUIRE(percent_encode("é") == "%C3%A9");
  REQUIRE(percent_encode(std::string{"a\0b", 3}) == "a%00b");
}

TEST_CASE("percent_decode: the escapes, and nothing for an invalid one", "[http][percent]") {
  REQUIRE(percent_decode("a%20b%2Fc%c3%a9") == "a b/cé");
  REQUIRE(percent_decode("a+b") == "a+b");
  REQUIRE_FALSE(percent_decode("%2"));
  REQUIRE_FALSE(percent_decode("%zz"));
  REQUIRE(percent_decode(percent_encode("any text: é/?&=+%")) == "any text: é/?&=+%");
}
