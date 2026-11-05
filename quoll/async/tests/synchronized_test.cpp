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

#include <quoll/async/detail/synchronized.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

using quoll::async::detail::synchronized;

TEST_CASE("synchronized: a function gets the value, and what it returns comes back", "[async][synchronized]") {
  synchronized<std::vector<int>> values{2U, 7};
  auto const size = values.with([](std::vector<int> &held) {
    held.push_back(8);
    return held.size();
  });

  REQUIRE(size == 3);

  auto const &read_only = values;

  REQUIRE(read_only.with([](std::vector<int> const &held) { return held.back(); }) == 8);
  values.with([](std::vector<int> &held) { held.clear(); });
  REQUIRE(read_only.with([](std::vector<int> const &held) { return held.empty(); }));
}

TEST_CASE("synchronized: a generic function gets the value with its constness", "[async][synchronized]") {
  synchronized<std::vector<int>> values;

  values.with([](auto &held) { held.push_back(1); });

  auto const &read_only = values;

  REQUIRE(read_only.with([](auto const &held) { return held.size(); }) == 1);
}

TEST_CASE("synchronized: the lock is released when the function throws", "[async][synchronized]") {
  synchronized<int> value{1};

  REQUIRE_THROWS_AS(value.with([](int &) -> void { throw std::runtime_error{"failed"}; }), std::runtime_error);
  REQUIRE(value.with([](int &held) { return held; }) == 1);
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("synchronized: threads do not race on the value", "[async][synchronized]") {
  constexpr std::size_t threads = 4;
  constexpr std::size_t rounds = 10'000;

  synchronized<std::size_t> count{};

  {
    std::vector<std::jthread> running;

    for (std::size_t index = 0; index < threads; ++index) {
      running.emplace_back([&count] {
        for (std::size_t round = 0; round < rounds; ++round) {
          count.with([](std::size_t &held) { ++held; });
        }
      });
    }
  }

  REQUIRE(count.with([](std::size_t const &held) { return held; }) == threads * rounds);
}

#endif
