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

#include <quoll/async/current_executor.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/detail/serve_websocket.hpp>
#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/io/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <utility>

namespace quoll::http {

namespace detail {

// What a connection's operations hold, each from when it is called.
struct websocket_state {
  websocket_state(io::any_stream connection, websocket_role end, websocket_options const &options)
      : stream(std::move(connection)), role(end), parser(end, options.max_message_size) {
    if (end == websocket_role::client) {
      masks = std::make_unique<async::detail::synchronized<std::random_device>>();
    }
  }

  io::any_stream stream;
  websocket_role role;

  // Receiving, under the turn `receiving` gives.
  std::atomic<bool> receiving = false;
  websocket_parser parser;
  // What ended receiving, which a receive() asked after it throws again.
  std::error_code failure;
  bool close_received = false;
  // Counts the reads that brought something, which tells a peer alive.
  std::atomic<std::uint64_t> activity = 0;

  // What the peer, or its stream, failed with first: what it throws through a handler is no failure of the handler's.
  async::detail::synchronized<std::error_code> peer_failure;

  // Sending: a frame at a time, in the order they are asked for. What follows is under that turn.
  async::detail::slots writes{1};
  // A close has been sent, or a frame cut short: nothing may follow.
  bool sending_over = false;
  // The stream's sending side is shut.
  bool sending_shut = false;

  // Where a client's masks come from, by any operation, on any thread; none for a server, which masks nothing.
  std::unique_ptr<async::detail::synchronized<std::random_device>> masks;
};

// What serve_websocket reaches of a connection.
struct websocket_access {
  [[nodiscard]] static std::shared_ptr<websocket_state> const &state_of(websocket const &socket) noexcept {
    return socket.state_;
  }
};

} // namespace detail

namespace {

using state = detail::websocket_state;

// `code` as the peer's failure, if it is the first: a stop is none.
void record_peer_failure(state &connection, std::error_code code) {
  if (code == std::errc::operation_canceled) {
    return;
  }

  connection.peer_failure.with([code](std::error_code &first) {
    if (!first) {
      first = code;
    }
  });
}

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
    record_peer_failure(*connection, sent.error);
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

// The close handshake over, a server ends its side first (RFC 6455, 7.1.1): the stream's FIN, or TLS's closing message,
// behind its close, so that the peer reads it all before the connection goes. Under the turn to send, from receive(),
// as a stream refuses it while a read or a write is pending.
async::task<> end_sending(std::shared_ptr<state> connection) {
  try {
    co_await connection->writes.acquire();

    write_turn const turn{connection->writes};

    if (!std::exchange(connection->sending_shut, true)) {
      static_cast<void>(co_await connection->stream.shutdown_send());
    }
  } catch (std::system_error const &) {
    // A stream gone, or a stop: the connection ends anyway.
  }
}

// The next message, its turn held.
async::task<websocket_message> receive_next(std::shared_ptr<state> connection) {
  // In the frame of a receive, as the HTTP path reads: a quiet connection waits in one.
  std::array<char, 4096> chunk;

  for (;;) {
    auto parsed = parse_next(connection->parser);

    if (!parsed) {
      record_peer_failure(*connection, parsed.error());

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

        if (connection->role == websocket_role::server) {
          co_await end_sending(connection);
        }

        co_return std::move(*message);
      default:
        co_return std::move(*message);
      }
    }

    auto const received = co_await connection->stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (received.transferred > 0) {
      connection->activity.fetch_add(1);
      connection->parser.feed(std::string_view{chunk.data(), received.transferred});
    }

    // What came before it is read first.
    if (received.error && received.transferred == 0) {
      record_peer_failure(*connection, received.error);
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

[[nodiscard]] std::error_code peer_failure_of(state &connection) {
  return connection.peer_failure.with([](std::error_code const &first) { return first; });
}

// Whether `code`, thrown through a handler, is no failure of its own: a send after a close, the peer's or a drain's, or
// what the peer's failure led to, the handler stopped once the peer had gone included.
[[nodiscard]] bool comes_of_the_peer(state &connection, std::error_code code) {
  if (code == error::websocket_closed) {
    return true;
  }

  auto const failure = peer_failure_of(connection);

  return failure && (code == failure || code == std::errc::operation_canceled);
}

// comes_of_the_peer() for what `failure` holds: only a std::system_error may.
[[nodiscard]] bool caught_of_the_peer(state &connection, std::exception_ptr const &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (std::system_error const &thrown) {
    return comes_of_the_peer(connection, thrown.code());
  } catch (...) {
    return false;
  }
}

// `watch` belongs to serve_websocket's frame, which awaits this at once. Ends the watch once the handler has ended,
// however it ended.
async::task<> run_handler(std::function<async::task<>(websocket)> handler, websocket socket, std::stop_source &watch) {
  async::detail::stop_on_exit const ending{watch};

  co_await handler(std::move(socket));
}

// Whether `duration` passed, rather than a stop ending the wait.
async::task<bool> slept(std::chrono::nanoseconds duration) {
  try {
    co_await async::sleep_for(duration);
  } catch (std::system_error const &stopped) {
    if (stopped.code() != std::errc::operation_canceled) {
      throw;
    }

    co_return false;
  }

  co_return true;
}

// What a stop asks of a sleep: to wake it.
struct wake_up {
  std::stop_source *wake;

  void operator()() const noexcept { wake->request_stop(); }
};

// `frame` sent within `limit`, or the peer taken as gone, as one that takes nothing holds the connection; nothing once
// `ends` is stopped, the handler over.
async::task<> send_in_time(std::shared_ptr<state> connection, std::string frame, frame_kind kind,
                           std::chrono::nanoseconds limit, std::stop_token ends) {
  try {
    co_await async::detail::stop_scope<void>{async::with_timeout(limit, send_frame(connection, std::move(frame), kind)),
                                             ends};
  } catch (std::system_error const &thrown) {
    // The handler over: nothing is owed, whatever stopped it.
    if (ends.stop_requested()) {
      co_return;
    }

    if (thrown.code() == std::errc::timed_out) {
      record_peer_failure(*connection, thrown.code());
    }

    throw;
  }
}

// Pings the peer once it has been quiet for `interval`, and fails once it has stayed so for another, which stops the
// handler beside this; sends a close with 1001 when the server drains. Runs until `ends` is stopped, and only with a
// timer to wait with.
async::task<> keep_alive(std::shared_ptr<state> connection, std::chrono::nanoseconds interval, std::stop_token drain,
                         std::stop_token ends) {
  if (!(co_await async::current_executor()).query<async::timer_ref>()) {
    co_return;
  }

  auto const pings = interval > std::chrono::nanoseconds::zero();
  // With no ping, only a drain or the end wakes it.
  auto const period = pings ? interval : std::chrono::nanoseconds{std::chrono::hours{24}};
  auto seen = connection->activity.load();
  auto pinged = false;
  auto drained = false;

  while (!ends.stop_requested()) {
    std::stop_source wake;
    std::stop_callback const on_end{ends, wake_up{&wake}};
    std::optional<std::stop_callback<wake_up>> on_drain;

    if (!drained) {
      on_drain.emplace(drain, wake_up{&wake});
    }

    auto const quiet = co_await async::detail::stop_scope<bool>{slept(period), wake.get_token()};

    if (ends.stop_requested()) {
      co_return;
    }

    if (!drained && drain.stop_requested()) {
      drained = true;

      co_await send_in_time(connection, format_websocket_close(1001, {}, mask_of(*connection)), frame_kind::close,
                            period, ends);
      continue;
    }

    if (!quiet || !pings) {
      continue;
    }

    auto const now = connection->activity.load();

    if (now != seen) {
      seen = now;
      pinged = false;
      continue;
    }

    if (pinged) {
      auto const silent = std::make_error_code(std::errc::timed_out);

      record_peer_failure(*connection, silent);
      throw std::system_error{silent, "quoll::http::websocket: the peer has gone quiet"};
    }

    co_await send_in_time(connection, format_websocket_frame(websocket_opcode::ping, {}, true, mask_of(*connection)),
                          frame_kind::answer, period, ends);
    pinged = true;
  }
}

// The peer's messages, dropped, up to its close: what the handler left unread.
async::task<> read_to_close(std::shared_ptr<state> connection) {
  while ((co_await receive_next(connection)).opcode != websocket_opcode::close) {
  }
}

// The connection, once its handler is over: a close if it sent none, 1011 if `failed` (RFC 6455, 7.4.1), then what the
// peer sends, read up to its close, unless receiving is over or still another's, a task left behind.
async::task<> close_and_read(std::shared_ptr<state> connection, bool failed, bool read) {
  co_await send_frame(connection, format_websocket_close(failed ? 1011 : 1000, {}, mask_of(*connection)),
                      frame_kind::close);

  if (!read || connection->receiving.exchange(true)) {
    co_return;
  }

  if (!connection->close_received && !connection->failure) {
    try {
      co_await read_to_close(connection);
    } catch (std::system_error const &) {
      // The peer gone meanwhile.
    }
  }

  connection->receiving.store(false);
}

// close_and_read(), within `limit` and only with a timer to wait with, then the stream's sending side shut, so that the
// connection ends behind the close rather than with a reset that may lose it.
async::task<> finish(std::shared_ptr<state> connection, bool failed, std::chrono::nanoseconds limit) {
  auto const timed = limit > std::chrono::nanoseconds::zero() &&
                     (co_await async::current_executor()).query<async::timer_ref>().has_value();

  try {
    if (timed) {
      co_await async::with_timeout(limit, close_and_read(connection, failed, true));
    } else {
      co_await close_and_read(connection, failed, false);
    }
  } catch (std::system_error const &) {
    // The peer slow to answer, or gone, or a stop: the connection ends all the same.
  }

  try {
    if (timed) {
      co_await async::with_timeout(limit, end_sending(connection));
    } else {
      co_await end_sending(connection);
    }
  } catch (std::system_error const &) {
    // Nor does a stream that does not take it hold it.
  }
}

} // namespace

namespace detail {

async::task<> serve_websocket(std::function<async::task<>(websocket)> handler, io::any_stream stream,
                              std::string received, websocket_options options, std::stop_token drain) {
  websocket socket{std::move(stream), websocket_role::server, std::move(received), options};
  auto const connection = websocket_access::state_of(socket);
  std::stop_source watch;
  std::exception_ptr failure;

  try {
    co_await async::when_all(run_handler(std::move(handler), std::move(socket), watch),
                             keep_alive(connection, options.ping_interval, std::move(drain), watch.get_token()));
  } catch (...) {
    failure = std::current_exception();
  }

  // A peer that has failed is owed nothing more, and would only hold the connection longer.
  auto const of_the_peer = failure && caught_of_the_peer(*connection, failure);

  if (!peer_failure_of(*connection)) {
    co_await finish(connection, failure && !of_the_peer, options.ping_interval);
  }

  if (failure && !of_the_peer) {
    std::rethrow_exception(failure);
  }
}

} // namespace detail

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
