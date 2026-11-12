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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/client/client.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/platform_ref.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;

using async::task;

// A body in `pieces`, which says when it goes.
struct canned_body {
  std::vector<std::string> pieces;
  bool *gone;
  std::size_t next = 0;

  canned_body(std::vector<std::string> given, bool *released) : pieces(std::move(given)), gone(released) {}

  canned_body(canned_body const &) = delete;
  canned_body &operator=(canned_body const &) = delete;
  canned_body(canned_body &&) = delete;
  canned_body &operator=(canned_body &&) = delete;

  ~canned_body() { *gone = true; }

  [[nodiscard]] task<std::optional<std::string_view>> read() {
    if (next == pieces.size()) {
      co_return std::nullopt;
    }

    // A server gone quiet.
    if (pieces[next] == "hang") {
      using namespace std::chrono_literals;

      co_await async::sleep_for(1h);
    }

    co_return std::string_view{pieces[next++]};
  }
};

// Says what it was given, and answers with what it holds.
struct canned_platform {
  std::string reason = "OK";
  http::fields fields{};
  std::vector<std::string> pieces{};
  mutable std::string url{};
  mutable std::string body{};
  mutable bool gone = false;
  bool hangs = false;

  [[nodiscard]] task<http::streamed_response> stream(std::string where, http::request request) const {
    using namespace std::chrono_literals;

    url = std::move(where);
    body = std::move(request.body);

    if (hangs) {
      co_await async::sleep_for(1h);
    }

    co_return http::streamed_response{
        http::status::ok,
        reason,
        http::version::http_1_1,
        fields,
        http::response_body{std::make_shared<canned_body>(pieces, &gone)},
        std::make_shared<http::fields const>()
    };
  }
};

static_assert(http::Platform<canned_platform>);

class platform_executor {
public:
  using services = async::service_list<async::timer_ref, http::platform_ref>;

  platform_executor(quoll::test::virtual_time &time, canned_platform const &platform) noexcept
      : time_(time.get_executor()), platform_(&platform) {}

  void post(async::work_item &item) const noexcept { time_.post(item); }

  [[nodiscard]] async::timer_ref query(async::service_tag<async::timer_ref> tag) const { return time_.query(tag); }

  [[nodiscard]] http::platform_ref query(async::service_tag<http::platform_ref>) const {
    return http::platform_ref{*platform_};
  }

  [[nodiscard]] bool operator==(platform_executor const &) const noexcept = default;

private:
  // Here, as the timer it gives refers to it.
  quoll::test::virtual_time::executor time_;
  canned_platform const *platform_;
};

template <class T>
T run_on(canned_platform const &platform, task<T> work) {
  quoll::test::virtual_time time;
  std::optional<T> result;
  std::exception_ptr failure;

  async::co_spawn(
      platform_executor{time, platform}, std::move(work),
      quoll::test::overloaded{
          [&](T value) { result = std::move(value); }, [&](std::exception_ptr thrown) { failure = std::move(thrown); }
      }
  );
  time.run();

  if (failure) {
    std::rethrow_exception(failure);
  }

  return std::move(result).value();
}

http::request get_of(std::string target) {
  http::request request;

  request.target = std::move(target);
  return request;
}

std::error_code code_of(auto &&call) {
  try {
    call();
  } catch (std::system_error const &failure) {
    return failure.code();
  }

  return {};
}

} // namespace

TEST_CASE("platform: a URL says where a request goes", "[client]") {
  auto const url_from = [](std::string_view base) {
    canned_platform const platform;
    auto const backend = http::detail::make_platform_backend(base, {});

    (void)run_on(platform, backend.operations->send(backend.state, get_of("/x?y=1")));
    return platform.url;
  };

  REQUIRE(url_from("") == "/x?y=1");
  REQUIRE(url_from("/api/v1") == "/api/v1/x?y=1");
  REQUIRE(url_from("/api/v1/") == "/api/v1/x?y=1");
  REQUIRE(url_from("http://example.com") == "http://example.com/x?y=1");
  REQUIRE(url_from("https://example.com:8443/v1") == "https://example.com:8443/v1/x?y=1");
  REQUIRE(url_from(".") == "./x?y=1");
  REQUIRE(url_from("./api") == "./api/x?y=1");
}

TEST_CASE("platform: a URL that would not read as itself is refused", "[client]") {
  REQUIRE_THROWS_AS(http::detail::make_platform_backend("/api/../v1", {}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::detail::make_platform_backend("ftp://example.com", {}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::detail::make_platform_backend("https://", {}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::detail::make_platform_backend("./../v1", {}), std::invalid_argument);
  REQUIRE_THROWS_AS(http::detail::make_platform_backend("http://exa mple.com", {}), std::invalid_argument);
}

TEST_CASE("platform: a target cannot leave the page's origin", "[client]") {
  auto const page = http::detail::make_platform_backend("", {});

  REQUIRE_THROWS_AS((void)page.operations->send(page.state, get_of("//evil.example/x")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)page.operations->send(page.state, get_of("/\\evil.example/x")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)page.operations->send(page.state, get_of("/\t/evil.example/x")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)page.operations->stream(page.state, get_of("//evil.example/x")), std::invalid_argument);

  // Past a path or an origin, it is a path.
  auto const under_a_path = http::detail::make_platform_backend("/api", {});
  auto const elsewhere = http::detail::make_platform_backend("https://example.com", {});

  REQUIRE_NOTHROW((void)under_a_path.operations->send(under_a_path.state, get_of("//x")));
  REQUIRE_NOTHROW((void)elsewhere.operations->send(elsewhere.state, get_of("//x")));

  auto const beside_the_page = http::detail::make_platform_backend(".", {});

  REQUIRE_NOTHROW((void)beside_the_page.operations->send(beside_the_page.state, get_of("//x")));
}

TEST_CASE("platform: a request is refused as a connection would, before any task", "[client]") {
  auto const backend = http::detail::make_platform_backend("https://example.com", {});
  auto split = get_of("/");

  split.fields.add("X-Test", "a\nX-Injected: 1");

  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, get_of("no-slash")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, get_of("/with a space")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, std::move(split)), std::invalid_argument);
  REQUIRE_THROWS_AS(
      (void)backend.operations->send(backend.state, get_of(std::string{"/a\0b", 4})), std::invalid_argument
  );
}

TEST_CASE("platform: a whole body comes in pieces, with its head", "[client]") {
  canned_platform platform;

  platform.reason = "Fine";
  platform.fields.add("x-test", "yes");
  platform.pieces = {"abc", "def"};

  auto const backend = http::detail::make_platform_backend("", {.limits = {.max_body_size = 6}});
  auto const response = run_on(platform, backend.operations->send(backend.state, get_of("/")));

  REQUIRE(response.status == http::status::ok);
  REQUIRE(response.reason == "Fine");
  REQUIRE(response.fields.find("x-test") == "yes");
  REQUIRE(response.body == "abcdef");
}

TEST_CASE("platform: a whole body past its limit lets the fetch go", "[client]") {
  canned_platform platform;

  platform.pieces = {"abc", "def"};

  auto const backend = http::detail::make_platform_backend("", {.limits = {.max_body_size = 5}});

  REQUIRE(code_of([&] {
            (void)run_on(platform, backend.operations->send(backend.state, get_of("/")));
          }) == http::error::body_too_large);
  REQUIRE(platform.gone);
}

TEST_CASE("platform: a body read in pieces fails for good past its limit", "[client]") {
  canned_platform platform;

  platform.pieces = {"abc", "defghi", "j"};

  auto const backend = http::detail::make_platform_backend("", {.limits = {.max_streamed_body_size = 4}});

  auto const read = [&]() -> task<std::vector<std::error_code>> {
    auto response = co_await backend.operations->stream(backend.state, get_of("/"));
    std::vector<std::error_code> errors;

    auto const first = co_await response.body.read();

    REQUIRE(first == "abc");

    for (auto attempt = 0; attempt < 2; ++attempt) {
      try {
        (void)co_await response.body.read();
        errors.emplace_back();
      } catch (std::system_error const &failure) {
        errors.push_back(failure.code());
      }

      REQUIRE(platform.gone);
    }

    co_return errors;
  };

  auto const errors = run_on(platform, read());

  REQUIRE(errors.size() == 2);
  REQUIRE(errors[0] == http::error::body_too_large);
  REQUIRE(errors[1] == http::error::body_too_large);
}

TEST_CASE("platform: the head has its limits, its status text counted", "[client]") {
  canned_platform platform;

  platform.reason = "OK";
  platform.fields.add("a", "b");

  auto const answered = [&](http::parse_limits limits) {
    auto const backend = http::detail::make_platform_backend("", {.limits = limits});

    return code_of([&] { (void)run_on(platform, backend.operations->send(backend.state, get_of("/"))); });
  };

  REQUIRE_FALSE(answered({.max_header_size = 4}));
  REQUIRE(answered({.max_header_size = 3}) == http::error::header_too_large);
  REQUIRE_FALSE(answered({.max_field_count = 1}));
  REQUIRE(answered({.max_field_count = 0}) == http::error::header_too_large);
}

TEST_CASE("platform: a writer has run by the time it reaches the platform", "[client]") {
  canned_platform const platform;
  auto request = get_of("/");

  request.method = http::verb::post;
  request.body_writer = [](http::body_stream out) -> task<> {
    co_await out.write("from ");
    co_await out.write("a writer");
  };

  auto const backend = http::detail::make_platform_backend("", {});

  (void)run_on(platform, backend.operations->send(backend.state, std::move(request)));

  REQUIRE(platform.body == "from a writer");
}

TEST_CASE("platform: an executor that does not offer one is refused", "[client]") {
  auto const backend = http::detail::make_platform_backend("", {});
  async::run_loop loop;
  std::exception_ptr failure;

  async::co_spawn(
      loop.get_executor(), backend.operations->send(backend.state, get_of("/")),
      quoll::test::overloaded{
          [](http::response const &) {}, [&](std::exception_ptr thrown) { failure = std::move(thrown); }
      }
  );
  loop.run();

  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
}

TEST_CASE("platform: what fetch refuses is refused before any task", "[client]") {
  auto const backend = http::detail::make_platform_backend("", {});

  auto const of = [](http::verb method, std::string body = {}) {
    auto request = get_of("/");

    request.method = method;
    request.body = std::move(body);
    return request;
  };

  auto with_writer = of(http::verb::get);

  with_writer.body_writer = [](http::body_stream) -> task<> { co_return; };

  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, of(http::verb::get, "a")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, of(http::verb::head, "a")), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, std::move(with_writer)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->send(backend.state, of(http::verb::connect)), std::invalid_argument);
  REQUIRE_THROWS_AS((void)backend.operations->stream(backend.state, of(http::verb::trace)), std::invalid_argument);
  REQUIRE_NOTHROW((void)backend.operations->send(backend.state, of(http::verb::post, "a")));
}

TEST_CASE("platform: a stream's head has its limits too", "[client]") {
  canned_platform platform;

  platform.fields.add("a", "b");

  auto const backend = http::detail::make_platform_backend("", {.limits = {.max_field_count = 0}});

  auto const open = [&]() -> task<int> {
    (void)co_await backend.operations->stream(backend.state, get_of("/"));
    co_return 1;
  };

  REQUIRE(code_of([&] { (void)run_on(platform, open()); }) == http::error::header_too_large);
}

TEST_CASE("platform: a response that does not come times out", "[client][timer]") {
  using namespace std::chrono_literals;

  canned_platform platform;

  platform.hangs = true;

  auto const backend = http::detail::make_platform_backend("", {.response_timeout = 5s});

  auto const open = [&]() -> task<int> {
    (void)co_await backend.operations->stream(backend.state, get_of("/"));
    co_return 1;
  };

  REQUIRE(code_of([&] {
            (void)run_on(platform, backend.operations->send(backend.state, get_of("/")));
          }) == std::errc::timed_out);
  REQUIRE(code_of([&] { (void)run_on(platform, open()); }) == std::errc::timed_out);
}

TEST_CASE("platform: a whole body that stops coming times out, and lets the fetch go", "[client][timer]") {
  using namespace std::chrono_literals;

  canned_platform platform;

  platform.pieces = {"abc", "hang"};

  auto const backend = http::detail::make_platform_backend("", {.response_timeout = 5s});

  REQUIRE(code_of([&] {
            (void)run_on(platform, backend.operations->send(backend.state, get_of("/")));
          }) == std::errc::timed_out);
  REQUIRE(platform.gone);
}

TEST_CASE("platform: a piece that does not come times out for good", "[client][timer]") {
  using namespace std::chrono_literals;

  canned_platform platform;

  platform.pieces = {"abc", "hang", "never"};

  auto const backend = http::detail::make_platform_backend("", {.response_timeout = 5s});

  auto const read = [&]() -> task<std::vector<std::error_code>> {
    auto response = co_await backend.operations->stream(backend.state, get_of("/"));
    auto const first = co_await response.body.read();
    std::vector<std::error_code> errors;

    REQUIRE(first == "abc");

    for (auto attempt = 0; attempt < 2; ++attempt) {
      try {
        (void)co_await response.body.read();
        errors.emplace_back();
      } catch (std::system_error const &failure) {
        errors.push_back(failure.code());
      }

      REQUIRE(platform.gone);
    }

    co_return errors;
  };

  auto const errors = run_on(platform, read());

  REQUIRE(errors.size() == 2);
  REQUIRE(errors[0] == std::errc::timed_out);
  REQUIRE(errors[1] == std::errc::timed_out);
}
