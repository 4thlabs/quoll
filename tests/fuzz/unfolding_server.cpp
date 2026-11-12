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

// A connection of the server unfolding as the input says: the first byte tells how many of the next ones are the
// script of the stream (quoll/fuzz/unfolding.hpp), the rest is what the peer sends. Reads and writes come in pieces,
// later, failing or hanging until a timeout ends them, and a step may stop the connection as a whole. Under ASan and
// UBSan, the connection must end, whatever happens on the way.

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/fuzz/unfolding.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/route_match.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/server/serve_options.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace {

namespace http = quoll::http;

using namespace std::chrono_literals;

// A route of each kind a connection runs differently: answered at once, after reading the body, while reading it, after
// a wait, or throwing.
http::router make_router() {
  http::router router;

  router.get("/", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  router.post("/echo", [](http::request request) {
    http::response response;

    response.body = std::move(request.body);
    return response;
  });
  router.post("/stream", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream out) -> quoll::async::task<> {
      while (auto const piece = co_await body.read()) {
        co_await out.write(*piece);
      }
    };
    return response;
  });
  router.add(
      http::verb::get, "/slow", "slow",
      [](http::request, http::route_match) -> quoll::async::task<http::response> {
        co_await quoll::async::sleep_for(2s);
        co_return http::response{};
      }
  );
  router.get("/throws", []() -> http::response { throw std::runtime_error{"on purpose"}; });

  return router;
}

// Low limits, so that an input reaches them, and timeouts on every wait, which virtual time reaches at once.
http::serve_options make_options() {
  http::serve_options options;

  options.connection.limits = quoll::fuzz::low_limits;
  options.connection.read_timeout = 1s;
  options.connection.write_timeout = 1s;
  options.connection.idle_timeout = 3s;
  options.connection.lingering_timeout = 500ms;
  options.connection.lingering_time = 1s;
  return options;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  static http::server const server{make_router(), make_options()};

  auto const script_size = std::min<std::size_t>(data[0], size - 1);
  quoll::fuzz::script steps{std::span{data + 1, script_size}};
  auto const peer = quoll::fuzz::as_text(data + 1 + script_size, size - 1 - script_size);
  std::stop_source stop;

  steps.stop_with(stop);

  // A stop ends the connection on the stop, which is how it ends then.
  auto const serve = [&]() -> quoll::async::task<bool> {
    static_cast<void>(co_await quoll::async::stopped_as_optional(
        server.serve_connection(quoll::io::any_stream{quoll::fuzz::unfolding_stream{steps, peer}})
    ));
    co_return true;
  };

  quoll::test::virtual_time time;

  quoll::fuzz::require(quoll::test::run(time, serve(), stop.get_token()).has_value());
  return 0;
}
