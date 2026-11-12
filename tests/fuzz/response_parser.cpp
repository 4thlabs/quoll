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

// A response from any server, for any request method.

#include <quoll/fuzz/support.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/verb.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace http = quoll::http;

namespace {

// What a server sends is read by its clients. Serializing does not know the method answered, whose framing a server
// prepares first (a response to HEAD): only the other responses read back as they were.
void check_round_trip(http::response const &message, http::verb method) {
  if (method != http::verb::get) {
    static_cast<void>(quoll::fuzz::serialized(message));
    return;
  }

  if (auto const same = quoll::fuzz::read_back(message)) {
    quoll::fuzz::require(same->status == message.status);
    quoll::fuzz::require(message.reason.empty() || same->reason == message.reason);
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // The first byte: a request's method changes what a response may hold, and its upper bits say in what pieces the
  // bytes come. 0: all at once.
  auto const method = data[0] % 3 == 0 ? http::verb::head : data[0] % 3 == 1 ? http::verb::connect : http::verb::get;
  auto const piece = (data[0] >> 2U) == 0 ? size : std::size_t{data[0]} >> 2U;
  http::response_parser parser{quoll::fuzz::low_limits};

  parser.set_request_method(method);
  quoll::fuzz::feed_in_pieces(
      parser, quoll::fuzz::as_text(data + 1, size - 1), piece,
      [method](http::response const &message) { check_round_trip(message, method); }
  );
  quoll::fuzz::check_body_in_pieces<http::response_parser>(
      quoll::fuzz::as_text(data + 1, size - 1), piece, [method](auto &ready) { ready.set_request_method(method); }
  );
  return 0;
}
