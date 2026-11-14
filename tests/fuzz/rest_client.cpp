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

// What a REST client decodes from a server anyone may run: the first byte picks a status, the rest is the body.

#include <quoll/async/task.hpp>
#include <quoll/fuzz/api.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/transport.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace http = quoll::http;

namespace {

struct canned_transport {
  http::status status;
  std::string body;
  std::string_view content_type;

  [[nodiscard]] quoll::async::task<http::response> send(http::request) const {
    http::response response;

    response.status = status;
    response.body = body;
    response.fields.add("Content-Type", content_type);
    return answering(std::move(response));
  }

  [[nodiscard]] static quoll::async::task<http::response> answering(http::response response) { co_return response; }
};

static_assert(http::HttpTransport<canned_transport>);

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  constexpr std::array statuses{
      http::status::ok, http::status::created, http::status::not_found, http::status::internal_server_error
  };

  if (size == 0) {
    return 0;
  }

  quoll::rest::client<quoll::fuzz::api, canned_transport> client{canned_transport{
      statuses[data[0] % statuses.size()], std::string{quoll::fuzz::as_text(data + 1, size - 1)},
      (data[0] & 4U) != 0 ? "application/problem+json" : "application/json"
  }};

  // try_call reports what it cannot decode as a value: anything thrown is a bug.
  quoll::fuzz::require(quoll::test::run(client.try_call<quoll::fuzz::get>(1)).has_value());
  quoll::fuzz::require(quoll::test::run(client.try_call<quoll::fuzz::list>({})).has_value());
  quoll::fuzz::require(quoll::test::run(client.try_call<quoll::fuzz::create>({.text = "x"})).has_value());

  // call reads the detail of a problem out of what it refuses, and throws nothing but an http::http_error received.
  try {
    quoll::fuzz::require(quoll::test::run(client.call<quoll::fuzz::get>(1)).has_value());
  } catch (quoll::http::http_error const &) {
  }

  return 0;
}
