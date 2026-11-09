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

// The frames of a WebSocket connection as a peer sends them: however the bytes are cut, the same messages and the same
// failure come, and each message written again reads back as itself.

#include <quoll/fuzz/support.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/http/websocket_message.hpp>
#include <quoll/http/websocket_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace http = quoll::http;

namespace {

// Low, so that inputs reach it.
constexpr std::size_t max_message_size = 512;

struct outcome {
  std::vector<http::websocket_message> messages;
  std::error_code failure;

  bool operator==(outcome const &) const = default;
};

[[nodiscard]] outcome read_all(http::websocket_role role, std::string_view input, std::size_t piece) {
  http::websocket_parser parser{role, max_message_size};
  outcome result;

  try {
    for (std::size_t position = 0; position < input.size() && !parser.closed(); position += piece) {
      parser.feed(input.substr(position, piece));

      while (auto message = parser.next()) {
        result.messages.push_back(std::move(*message));
      }
    }
  } catch (std::system_error const &failure) {
    result.failure = failure.code();
  }

  return result;
}

// Each message written again, as the peer of `role` writes it, into one parser: the same messages.
void check_written_again(http::websocket_role role, outcome const &read) {
  std::optional<http::websocket_mask> mask;

  if (role == http::websocket_role::server) {
    mask = http::websocket_mask{1, 2, 3, 4};
  }

  std::string text;

  for (auto const &message : read.messages) {
    if (message.opcode == http::websocket_opcode::close) {
      text += message.close_code == 1005 ? http::format_websocket_frame(message.opcode, {}, true, mask)
                                         : http::format_websocket_close(message.close_code, message.data, mask);
    } else {
      text += http::format_websocket_frame(message.opcode, message.data, true, mask);
    }
  }

  auto const again = read_all(role, text, text.size() + 1);

  quoll::fuzz::require(!again.failure);
  quoll::fuzz::require(again.messages == read.messages);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // The first byte: its top bit the role, the rest in what pieces the bytes come, 0 all at once.
  auto const role = (data[0] & 0x80U) != 0 ? http::websocket_role::server : http::websocket_role::client;
  auto const cut = std::size_t{data[0] & 0x7FU};
  auto const input = quoll::fuzz::as_text(data + 1, size - 1);
  auto const whole = read_all(role, input, input.size() + 1);

  quoll::fuzz::require(read_all(role, input, cut == 0 ? input.size() + 1 : cut) == whole);

  if (!whole.failure) {
    check_written_again(role, whole);
  }

  return 0;
}
