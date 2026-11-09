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

#include <quoll/http/websocket.hpp>

#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/io/write.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace quoll::http {

namespace detail {

// What a connection's operations hold, each from when it is called.
struct websocket_state {
  websocket_state(io::any_stream connection, websocket_role end, websocket_options const &options)
      : stream(std::move(connection)), parser(end, options.max_message_size) {
    if (end == websocket_role::client) {
      masks = std::make_unique<async::detail::synchronized<std::random_device>>();
    }
  }

  io::any_stream stream;

  // Receiving, under the turn `receiving` gives.
  std::atomic<bool> receiving = false;
  websocket_parser parser;
  std::array<char, 16 * 1024> chunk{};
  // What ended receiving, which a receive() asked after it throws again.
  std::error_code failure;
  bool close_received = false;

  // Sending: a frame at a time, in the order they are asked for. What follows is under that turn.
  async::detail::slots writes{1};
  // A close has been sent, or a frame cut short: nothing may follow.
  bool sending_over = false;

  // Where a client's masks come from, by any operation, on any thread; none for a server, which masks nothing.
  std::unique_ptr<async::detail::synchronized<std::random_device>> masks;
};

} // namespace detail

namespace {

using state = detail::websocket_state;

// What a frame is to what sends it once nothing more may be sent: a message refused, an answer or a close dropped.
enum class frame_kind : std::uint8_t { message, answer, close };

// The turn to send, taken: given back as it goes.
class write_turn {
public:
  explicit write_turn(async::detail::slots &writes) noexcept : writes_(&writes) {}

  write_turn(write_turn const &) = delete;
  write_turn &operator=(write_turn const &) = delete;
  write_turn(write_turn &&) = delete;
  write_turn &operator=(write_turn &&) = delete;

  ~write_turn() { writes_->release(); }

private:
  async::detail::slots *writes_;
};

// The mask of a client's next frame, from a strong source of randomness (RFC 6455, 5.3); none for a server.
[[nodiscard]] std::optional<websocket_mask> mask_of(state &connection) {
  if (!connection.masks) {
    return std::nullopt;
  }

  auto const bits =
      connection.masks->with([](std::random_device &source) { return static_cast<std::uint32_t>(source()); });

  return websocket_mask{static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8U),
                        static_cast<std::uint8_t>(bits >> 16U), static_cast<std::uint8_t>(bits >> 24U)};
}

// `frame` sent, once those asked before it have left.
async::task<> send_frame(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  co_await connection->writes.acquire();

  write_turn const turn{connection->writes};

  if (connection->sending_over) {
    if (kind == frame_kind::message) {
      throw std::system_error{make_error_code(error::websocket_closed), "quoll::http::websocket: cannot send"};
    }

    co_return;
  }

  // Before it is written: a frame cut short leaves the stream in its middle, where no other may follow.
  connection->sending_over = true;

  auto const sent = co_await io::write(connection->stream, std::as_bytes(std::span{frame}));

  if (sent.error) {
    throw std::system_error{sent.error, "quoll::http::websocket: cannot send"};
  }

  connection->sending_over = kind == frame_kind::close;
}

// What the parser reads next, or how the peer broke the protocol.
[[nodiscard]] std::expected<std::optional<websocket_message>, std::error_code> parse_next(websocket_parser &parser) {
  try {
    return parser.next();
  } catch (std::system_error const &broken) {
    return std::unexpected{broken.code()};
  }
}

// The close that answers the peer's: its code, but for none, which is answered with none.
[[nodiscard]] std::string close_answering(websocket_message const &close, std::optional<websocket_mask> mask) {
  if (close.close_code == 1005) {
    return format_websocket_frame(websocket_opcode::close, {}, true, mask);
  }

  return format_websocket_close(close.close_code, {}, mask);
}

// The next message, its turn held.
async::task<websocket_message> receive_next(std::shared_ptr<state> connection) {
  for (;;) {
    auto parsed = parse_next(connection->parser);

    if (!parsed) {
      // Told, if it can be: what fails sending it changes nothing to what is thrown.
      try {
        co_await send_frame(connection, format_websocket_close(close_code_of(parsed.error()), {}, mask_of(*connection)),
                            frame_kind::close);
      } catch (std::system_error const &) {
      }

      throw std::system_error{parsed.error(), "quoll::http::websocket: the peer broke the protocol"};
    }

    if (auto &message = *parsed) {
      switch (message->opcode) {
      case websocket_opcode::ping:
        co_await send_frame(connection,
                            format_websocket_frame(websocket_opcode::pong, message->data, true, mask_of(*connection)),
                            frame_kind::answer);
        continue;
      case websocket_opcode::pong:
        continue;
      case websocket_opcode::close:
        connection->close_received = true;

        // A peer that closed its stream after its close, as some do, has had all it needs.
        try {
          co_await send_frame(connection, close_answering(*message, mask_of(*connection)), frame_kind::close);
        } catch (std::system_error const &) {
        }

        co_return std::move(*message);
      default:
        co_return std::move(*message);
      }
    }

    auto const received = co_await connection->stream.read_some(std::as_writable_bytes(std::span{connection->chunk}));

    if (received.transferred > 0) {
      connection->parser.feed(std::string_view{connection->chunk.data(), received.transferred});
    }

    // What came before it is read first.
    if (received.error && received.transferred == 0) {
      throw std::system_error{received.error, "quoll::http::websocket: cannot receive"};
    }
  }
}

// `turn` until the frame goes, given back with it.
async::task<websocket_message> receive_from(std::shared_ptr<state> connection, detail::one_at_a_time turn) {
  auto const held = std::move(turn);

  if (connection->failure) {
    throw std::system_error{connection->failure, "quoll::http::websocket: receiving has failed"};
  }

  try {
    co_return co_await receive_next(connection);
  } catch (std::system_error const &thrown) {
    // Whatever it was in the middle of reading cannot be taken up again.
    connection->failure = thrown.code();
    throw;
  }
}

} // namespace

websocket::websocket(io::any_stream stream, websocket_role role, std::string received, websocket_options options)
    : state_(std::make_shared<detail::websocket_state>(std::move(stream), role, options)) {
  state_->parser.feed(received);
}

async::task<websocket_message> websocket::receive() {
  check_usable();

  detail::one_at_a_time turn{std::shared_ptr<std::atomic<bool>>{state_, &state_->receiving},
                             "quoll::http::websocket: a receive() while another is pending"};

  // Read under the turn, which orders it after what the last receive() did.
  if (state_->close_received) {
    throw std::logic_error{"quoll::http::websocket: a receive() after the close"};
  }

  return receive_from(state_, std::move(turn));
}

async::task<> websocket::send_text(std::string_view text) {
  check_usable();

  if (!detail::is_utf8(text)) {
    throw std::invalid_argument{"quoll::http::websocket: text that is not UTF-8"};
  }

  return send_frame(state_, format_websocket_frame(websocket_opcode::text, text, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::send_binary(std::string_view data) {
  check_usable();
  return send_frame(state_, format_websocket_frame(websocket_opcode::binary, data, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::ping(std::string_view data) {
  check_usable();
  return send_frame(state_, format_websocket_frame(websocket_opcode::ping, data, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::close(std::uint16_t code, std::string_view reason) {
  check_usable();
  return send_frame(state_, format_websocket_close(code, reason, mask_of(*state_)), frame_kind::close);
}

void websocket::check_usable() const {
  if (!state_) {
    throw std::logic_error{"quoll::http::websocket: used once moved from"};
  }
}

} // namespace quoll::http
