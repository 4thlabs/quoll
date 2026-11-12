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

// A response from a server anyone may run: bytes in pieces, read by a client. It gives a response, or
// throws what its documentation says, and nothing else.

#include <quoll/async/task.hpp>
#include <quoll/client/client.hpp>
#include <quoll/client/client_options.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct input_connector {
  std::string_view input;
  std::size_t chunk;

  [[nodiscard]] quoll::async::task<quoll::io::any_stream> connect(std::string, std::uint16_t) const {
    co_return quoll::io::any_stream{quoll::fuzz::input_stream{input, chunk}};
  }
};

static_assert(quoll::io::Connector<input_connector>);

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  constexpr std::array methods{
      quoll::http::verb::get, quoll::http::verb::head, quoll::http::verb::post, quoll::http::verb::get
  };

  if (size == 0) {
    return 0;
  }

  quoll::http::client_options options;

  options.limits = quoll::fuzz::low_limits;
  options.max_redirects = 2;

  // With a timer, as a client has one. A second send takes the connection the first one kept, or finds it closed.
  quoll::test::virtual_time time;
  quoll::http::client const client{
      input_connector{quoll::fuzz::as_text(data + 1, size - 1), std::size_t{data[0]} >> 4U}, "fuzz.example", 80, options
  };

  for (unsigned send = 0; send < 2; ++send) {
    quoll::http::request request;

    // Two bits a send: a POST takes a kept connection as one not sent twice, found quiet by its check, a HEAD reads no
    // body. The upper bits give the pieces.
    request.method = methods[(data[0] >> (2U * send)) & 3U];
    request.target = "/";

    try {
      quoll::fuzz::require(quoll::test::run(time, client.send(std::move(request))).has_value());
    } catch (std::system_error const &) {
    }
  }

  // The same bytes through stream(), read in pieces, to their end or up to what stops them.
  auto const reading = [&]() -> quoll::async::task<bool> {
    quoll::http::request request;

    // As above: a POST takes a kept connection as one not sent twice, a HEAD reads no body.
    request.method = methods[(data[0] >> 2U) & 3U];

    auto response = co_await client.stream(std::move(request));

    while (co_await response.body.read()) {
    }

    co_return true;
  };

  try {
    quoll::fuzz::require(quoll::test::run(time, reading()).has_value());
  } catch (std::system_error const &) {
  }

  return 0;
}
