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
#include <quoll/async/task.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/server/serve.hpp>
#include <quoll/test/overloaded.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::verb;
using quoll::async::task;
using quoll::test::overloaded;

// Hands over the server ends it was given, then fails with `last`: operation_canceled stops serve() as a stop request
// would.
struct canned_listener {
  std::vector<io::memory_stream> streams;
  std::error_code last = std::make_error_code(std::errc::operation_canceled);

  task<io::any_stream> accept() {
    if (streams.empty()) {
      throw std::system_error{last, "canned_listener"};
    }

    auto stream = std::move(streams.back());

    streams.pop_back();
    co_return io::any_stream{std::move(stream)};
  }
};

static_assert(io::Listener<canned_listener>);

// A client: one GET, its response's body.
task<std::string> get(io::memory_stream client, std::string target) {
  http::request request;
  std::string buffer;
  http::response_parser parser;

  request.target = std::move(target);
  request.fields.add("Host", "x");
  (void)co_await http::write(client, request);
  (void)co_await http::read(client, buffer, parser);
  co_return parser.release().body;
}

} // namespace

TEST_CASE("serve: each accepted connection is served concurrently", "[server]") {
  http::router router;

  router.add(verb::get, "/{name}", "echo", [](http::request, http::route_match match) -> task<http::response> {
    http::response response;

    response.body = match.params.front();
    co_return response;
  });

  quoll::async::run_loop loop;
  auto [first_client, first_server] = io::make_memory_stream_pair();
  auto [second_client, second_server] = io::make_memory_stream_pair();
  canned_listener listener;

  listener.streams.push_back(std::move(first_server));
  listener.streams.push_back(std::move(second_server));

  bool served = false;
  std::optional<std::string> first;
  std::optional<std::string> second;
  auto const store = [](std::optional<std::string> &into) {
    return overloaded{[&into](std::string body) { into = std::move(body); }, [](std::exception_ptr) {}};
  };

  quoll::async::co_spawn(loop.get_executor(), quoll::server::serve(loop.get_executor(), listener, router),
                         overloaded{[&] { served = true; }, [](std::exception_ptr) {}});
  quoll::async::co_spawn(loop.get_executor(), get(std::move(first_client), "/one"), store(first));
  quoll::async::co_spawn(loop.get_executor(), get(std::move(second_client), "/two"), store(second));
  loop.run();

  // The listener stopped with operation_canceled: serve() completed, the connections were served.
  REQUIRE(served);
  REQUIRE(first == "one");
  REQUIRE(second == "two");
}

TEST_CASE("serve: another failure to accept is rethrown", "[server]") {
  http::router const router;
  quoll::async::run_loop loop;
  canned_listener listener{.streams = {}, .last = std::make_error_code(std::errc::too_many_files_open)};
  std::exception_ptr failure;

  quoll::async::co_spawn(loop.get_executor(), quoll::server::serve(loop.get_executor(), listener, router),
                         overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }});
  loop.run();

  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::system_error);
}
