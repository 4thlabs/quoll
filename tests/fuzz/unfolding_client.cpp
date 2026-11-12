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

// An exchange of the client unfolding as the input says: the first byte tells how many bytes after the second are the
// script of the stream (quoll/fuzz/unfolding.hpp), the second what the two requests are, the rest is what the server
// answers. A request with a body written in
// pieces reads the response as it writes, a timeout ends what hangs, and a step may stop the exchange as a whole.
// Under ASan and UBSan, every send must end, with a response or an error.

#include <quoll/async/task.hpp>
#include <quoll/client/client.hpp>
#include <quoll/client/client_options.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/fuzz/unfolding.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace {

namespace http = quoll::http;

using namespace std::chrono_literals;

// Every connection reads the same answer, unfolding as the shared script goes on.
struct unfolding_connector {
  quoll::fuzz::script *steps;
  std::string_view answer;

  [[nodiscard]] quoll::async::task<quoll::io::any_stream> connect(std::string, std::uint16_t) const {
    co_return quoll::io::any_stream{quoll::fuzz::unfolding_stream{*steps, answer}};
  }
};

static_assert(quoll::io::Connector<unfolding_connector>);

http::request make_request(std::uint8_t kind) {
  http::request request;

  request.target = "/";

  switch (kind % 3) {
  case 0:
    request.method = http::verb::get;
    break;
  case 1:
    request.method = http::verb::post;
    request.body = "a body";
    break;
  default:
    // In pieces: the response is read meanwhile.
    request.method = http::verb::post;
    request.body_writer = [](http::body_stream out) -> quoll::async::task<> {
      for (int piece = 0; piece < 4; ++piece) {
        co_await out.write("piece");
      }
    };
  }

  return request;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size < 2) {
    return 0;
  }

  auto const kinds = data[1];
  auto const script_size = std::min<std::size_t>(data[0], size - 2);
  quoll::fuzz::script steps{std::span{data + 2, script_size}};
  auto const answer = quoll::fuzz::as_text(data + 2 + script_size, size - 2 - script_size);
  std::stop_source stop;

  steps.stop_with(stop);

  http::client_options options;

  options.limits = quoll::fuzz::low_limits;
  options.connect_timeout = 1s;
  options.response_timeout = 1s;
  options.max_redirects = 1;

  quoll::test::virtual_time time;
  http::client const client{unfolding_connector{&steps, answer}, "fuzz.example", 80, options};

  // Two sends, the second taking the connection the first kept, if any: their kinds from the second byte.
  // What a stream failing or a timeout throws, and a stop the send ends on, are codes there.
  for (unsigned send = 0; send < 2; ++send) {
    quoll::fuzz::require(
        quoll::test::run(
            time,
            quoll::test::code_thrown_by(client.send(make_request(static_cast<std::uint8_t>(kinds >> (2U * send))))),
            stop.get_token()
        )
            .has_value()
    );
  }

  return 0;
}
