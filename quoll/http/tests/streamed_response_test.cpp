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

#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;

using quoll::async::task;

// What a transport gives a response_body: pieces, and something to see its end by.
class pieces {
public:
  pieces(std::vector<std::string> left, bool &gone) noexcept : left_(std::move(left)), gone_(&gone) {}

  pieces(pieces const &) = delete;
  pieces(pieces &&) = delete;
  pieces &operator=(pieces const &) = delete;
  pieces &operator=(pieces &&) = delete;

  ~pieces() { *gone_ = true; }

  [[nodiscard]] task<std::optional<std::string_view>> read() {
    if (next_ == left_.size()) {
      co_return std::nullopt;
    }

    co_return std::string_view{left_[next_++]};
  }

private:
  std::vector<std::string> left_;
  std::size_t next_ = 0;
  bool *gone_;
};

task<std::string> read_all(http::response_body const &body) {
  std::string all;

  while (auto const piece = co_await body.read()) {
    all += *piece;
  }

  co_return all;
}

} // namespace

TEST_CASE("response_body reads its source, whichever transport gave it", "[http]") {
  quoll::async::run_loop loop;
  auto gone = false;

  SECTION("up to its end") {
    http::response_body const body{std::make_shared<pieces>(std::vector<std::string>{"ab", "c"}, gone)};

    REQUIRE(quoll::test::run(loop, read_all(body)) == "abc");
    REQUIRE(!gone);
  }

  SECTION("through its copies, as one") {
    http::response_body const body{std::make_shared<pieces>(std::vector<std::string>{"ab", "c"}, gone)};
    auto const copy = body;

    auto const reading = [&]() -> task<std::string> {
      auto const first = co_await body.read();
      auto seen = std::string{*first};

      seen += *(co_await copy.read());
      co_return seen;
    };

    REQUIRE(quoll::test::run(loop, reading()) == "abc");
  }

  SECTION("holding its source for as long as a copy lives") {
    std::optional<http::response_body> body{std::in_place,
                                            std::make_shared<pieces>(std::vector<std::string>{"ab"}, gone)};
    auto const copy = *body;

    body.reset();
    REQUIRE(!gone);
    REQUIRE(quoll::test::run(loop, read_all(copy)) == "ab");
  }

  SECTION("nothing, built by default or moved from") {
    http::response_body const nothing;
    http::response_body taken{std::make_shared<pieces>(std::vector<std::string>{"ab"}, gone)};
    auto const moved = std::move(taken);

    REQUIRE(quoll::test::run(loop, read_all(nothing)) == "");
    // NOLINTNEXTLINE(bugprone-use-after-move)
    REQUIRE(quoll::test::run(loop, read_all(taken)) == "");
    REQUIRE(quoll::test::run(loop, read_all(moved)) == "ab");
  }

  SECTION("nothing, from no source") {
    http::response_body const none{std::shared_ptr<pieces>{}};

    REQUIRE(quoll::test::run(loop, read_all(none)) == "");
  }

  SECTION("letting it go with its last copy") {
    {
      http::response_body const body{std::make_shared<pieces>(std::vector<std::string>{"ab"}, gone)};
      auto const copy = body;

      REQUIRE(!gone);
    }

    REQUIRE(gone);
  }
}

TEST_CASE("streamed_response: built by default, it has nothing to read and no trailers", "[http]") {
  quoll::async::run_loop loop;
  http::streamed_response const response;

  REQUIRE(response.status == http::status::ok);
  REQUIRE(quoll::test::run(loop, read_all(response.body)) == "");
  REQUIRE(response.trailers == nullptr);
}
