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

// What a REST route decodes, past the HTTP parser: the first line is the target, the second the value of its header
// fields, the rest the body. A route answers what it throws with a 500: the service never throws here, so a 500 is a
// bug in the decoding.

#include <quoll/fuzz/api.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/run.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace http = quoll::http;

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  constexpr std::array methods{http::verb::get, http::verb::post};
  constexpr std::array media_types{"application/json", "Application/JSON; charset=x", "text/plain", ""};

  if (size == 0) {
    return 0;
  }

  auto input = quoll::fuzz::as_text(data + 1, size - 1);
  auto const take_line = [&input] {
    auto const end = input.find('\n');
    auto const line = input.substr(0, end);

    input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
    return line;
  };
  auto const target = take_line();
  auto const field = take_line();
  http::request request;

  request.method = methods[data[0] % methods.size()];
  request.target = "/notes" + std::string{target};

  // The first byte says how often each field comes, and in what media type the body is: one bit, two bits.
  auto const lines = 1 + ((data[0] >> 1U) & 1U);

  for (unsigned line = 0; line < lines; ++line) {
    for (auto const *const name : {"X-Tags", "X-Count", "X-Api-Key", "Authorization"}) {
      request.fields.add(name, field);
    }
  }

  if (!input.empty()) {
    request.body = std::string{input};

    if (auto const *const media_type = media_types[(data[0] >> 2U) % media_types.size()]; *media_type != '\0') {
      request.fields.add("Content-Type", media_type);
    }
  }

  auto const response = quoll::test::run(quoll::fuzz::routes().dispatch(std::move(request)));

  quoll::fuzz::require(response.has_value());
  quoll::fuzz::require(response->status != http::status::internal_server_error);
  return 0;
}
