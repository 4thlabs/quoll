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
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/code_of.hpp>
#include <quoll/async/detail/held_place.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/http/detail/one_at_a_time.hpp>
#include <quoll/http/detail/serve_websocket.hpp>
#include <quoll/http/detail/utf8.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/format_websocket_frame.hpp>
#include <quoll/io/io_result.hpp>

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

// Where sending is, under the turn to send: once it is not open, nothing more may be sent.
enum class sending_state : std::uint8_t {
  open,
  // A frame cut short, which leaves the stream in its middle.
  cut,
  // A close has left whole.
  closed,
};

// What a connection's operations hold, each from when it is called.
struct websocket_state {
  websocket_state(io::any_stream connection, websocket_role end, websocket_options const &options)
      : stream(std::move(connection)), role(end), parser(end, options.max_message_size),
        wait_limit(options.ping_interval > std::chrono::nanoseconds::zero()
                       ? options.ping_interval
                       : std::chrono::nanoseconds{std::chrono::seconds{30}}) {
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
  // Counts the reads that brought something: what tells a peer alive.
  std::atomic<std::uint64_t> reads_done = 0;
  std::atomic<bool> close_came = false;

  // Sending: a frame at a time, in the order they are asked for. What follows is under that turn.
  async::detail::slots writes{1};
  sending_state sending = sending_state::open;
  // The stream's sending side is shut.
  bool sending_shut = false;

  // Stopped when a close leaves or comes, which wakes the keep-alive.
  std::stop_source closing;
  // The stop of the connection a server runs this in, which what is sent whole follows; none outside a server.
  std::stop_token connection_stop;

  // How long a write may wait with nothing taken, past which the peer is taken as gone: `ping_interval`, or 30 seconds
  // where it is zero, so that a peer that no longer reads holds neither the connection nor the turn to send, which all
  // the rest waits for. Only with a timer.
  std::chrono::nanoseconds wait_limit;

  // Where a client's masks come from, by any operation; none for a server, which masks nothing.
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

// What a frame is to what sends it once nothing more may be sent: a message refused, an answer or a close dropped.
enum class frame_kind : std::uint8_t { message, answer, close };

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

// A frame of `opcode` with `data`, masked as the connection's role asks.
[[nodiscard]] std::string frame_of(state &connection, websocket_opcode opcode, std::string_view data) {
  return format_websocket_frame(opcode, data, true, mask_of(connection));
}

// A close with `code` and `reason`, masked as the connection's role asks.
[[nodiscard]] std::string close_of(state &connection, std::uint16_t code, std::string_view reason = {}) {
  return format_websocket_close(code, reason, mask_of(connection));
}

// One write of `rest`, which the caller holds, awaiting this at once.
async::task<io::io_result> write_once(std::shared_ptr<state> connection, std::span<std::byte const> rest) {
  co_return co_await connection->stream.write_some(rest);
}

// `frame` written, the turn to send held. A loop of its own rather than io::write(): each write may wait wait_limit
// with nothing taken, past which the send throws std::errc::timed_out, so that no turn is held longer than the peer
// goes on taking.
async::task<> write_frame(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  if (connection->sending != detail::sending_state::open) {
    if (kind == frame_kind::message) {
      throw std::system_error{make_error_code(error::websocket_closed), "quoll::http::websocket: cannot send"};
    }

    co_return;
  }

  auto rest = std::as_bytes(std::span{frame});

  while (!rest.empty()) {
    auto const sent =
        co_await async::detail::with_optional_timeout(connection->wait_limit, write_once(connection, rest));
    auto const fits = sent.transferred <= rest.size();

    // From its first byte, the frame is under way, until it has left whole.
    if (fits && sent.transferred > 0) {
      connection->sending = detail::sending_state::cut;
      rest = rest.subspan(sent.transferred);
    }

    // A stream that takes nothing, or more than it was given, without saying why, breaks its contract: see io::write.
    if (sent.error || sent.transferred == 0 || !fits) {
      // A stop that is not the operation's is the limit's.
      auto const timed_out =
          sent.error == std::errc::operation_canceled && !(co_await async::current_stop_token()).stop_requested();
      auto const failure = timed_out    ? std::make_error_code(std::errc::timed_out)
                           : sent.error ? sent.error
                                        : std::make_error_code(std::errc::io_error);

      // A peer that takes nothing is gone: no other write waits for it again.
      if (timed_out) {
        connection->sending = detail::sending_state::cut;
      }

      throw std::system_error{failure, "quoll::http::websocket: cannot send"};
    }
  }

  if (kind == frame_kind::close) {
    connection->sending = detail::sending_state::closed;
    connection->closing.request_stop();
  } else {
    connection->sending = detail::sending_state::open;
  }
}

// What the handler sends, once what was asked before it has left, under the stop of the operation that sends it.
async::task<> send_message(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  async::detail::held_place turn;

  co_await turn.take(connection->writes);
  co_await write_frame(connection, std::move(frame), kind);
}

// What a stop asks of a wait: to end it.
struct wake_up {
  std::stop_source *wake;

  void operator()() const noexcept { wake->request_stop(); }
};

// What the connection sends on its own, a pong, a close, a ping: whole, rather than stopped by the operation it serves,
// as an answer cut short would leave the peer unanswered, or the stream in the middle of a frame; but stopped by the
// connection's stop, and its wait for the turn by `turn_stop`. What holds the turn, and its own writes, take no longer
// than wait_limit with nothing taken. False if it could not leave, a stop of `turn_stop` aside: the stream's failure,
// which the next operation finds out.
async::task<bool> send_quietly(std::shared_ptr<state> connection, std::string frame, frame_kind kind,
                               std::stop_token turn_stop = {}) {
  auto const sending = [](std::shared_ptr<state> socket, std::string bytes, frame_kind what,
                          std::stop_token stop) -> async::task<> {
    // The turn waited for until either stop: stop_scope replaces the connection's stop rather than add to it.
    std::stop_source either;
    std::stop_callback const on_connection{socket->connection_stop, wake_up{&either}};
    std::stop_callback const on_turn{stop, wake_up{&either}};
    async::detail::held_place turn;

    co_await async::detail::stop_scope<void>{turn.take(socket->writes), either.get_token()};

    // Given as the stop came: what it was asked for no longer is.
    if (stop.stop_requested()) {
      co_return;
    }

    co_await write_frame(socket, std::move(bytes), what);
  };

  // Copied before the state is given to what it stops.
  auto connection_stop = connection->connection_stop;

  try {
    co_await async::detail::stop_scope<void>{sending(std::move(connection), std::move(frame), kind, turn_stop),
                                             std::move(connection_stop)};
  } catch (std::system_error const &) {
    co_return turn_stop.stop_requested();
  }

  co_return true;
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
[[nodiscard]] std::string close_answering(state &connection, websocket_message const &close) {
  if (close.close_code == 1005) {
    return frame_of(connection, websocket_opcode::close, {});
  }

  return close_of(connection, close.close_code);
}

// The server's side shut behind its close (RFC 6455, 7.1.1): the stream's FIN, or TLS's closing message. Under the turn
// to send, while nothing reads, as a stream refuses it while a read or a write is pending; within wait_limit.
async::task<> end_sending(std::shared_ptr<state> connection) {
  auto const shutting = [](std::shared_ptr<state> socket) -> async::task<> {
    async::detail::held_place turn;

    co_await turn.take(socket->writes);

    if (socket->sending_shut) {
      co_return;
    }

    // Nothing may be written past it, a close that could not leave included.
    if (socket->sending == detail::sending_state::open) {
      socket->sending = detail::sending_state::cut;
    }

    // Done once it has left: one stopped, or failed, may be tried again, by finish().
    socket->sending_shut = !(co_await socket->stream.shutdown_send()).error;
  };

  try {
    auto const limit = connection->wait_limit;

    co_await async::detail::with_optional_timeout(limit, shutting(std::move(connection)));
  } catch (std::system_error const &) {
    // A stream gone, slow, or a stop: the connection ends anyway.
  }
}

// The next message, its turn held.
async::task<websocket_message> receive_next(std::shared_ptr<state> connection) {
  // In the frame of a receive, as the HTTP path reads: a quiet connection waits in one.
  std::array<char, 4096> chunk;

  for (;;) {
    auto parsed = parse_next(connection->parser);

    if (!parsed) {
      // Told, if it can be: what fails sending it changes nothing to what is thrown.
      static_cast<void>(co_await send_quietly(connection, close_of(*connection, close_code_of(parsed.error())),
                                              frame_kind::close, co_await async::current_stop_token()));

      throw std::system_error{parsed.error(), "quoll::http::websocket: the peer broke the protocol"};
    }

    if (auto &message = *parsed) {
      switch (message->opcode) {
      case websocket_opcode::ping:
        // Its wait for the turn ends with the receive()'s stop too: a stopped receive() leaves its pong unsent.
        static_cast<void>(co_await send_quietly(connection,
                                                frame_of(*connection, websocket_opcode::pong, message->data),
                                                frame_kind::answer, co_await async::current_stop_token()));
        continue;
      case websocket_opcode::pong:
        continue;
      case websocket_opcode::close:
        connection->close_came.store(true);
        connection->closing.request_stop();

        // A peer that closed its stream after its close, as some do, has had all it needs.
        static_cast<void>(co_await send_quietly(connection, close_answering(*connection, *message), frame_kind::close,
                                                co_await async::current_stop_token()));

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
      connection->reads_done.fetch_add(1);
      connection->parser.feed(std::string_view{chunk.data(), received.transferred});
      continue;
    }

    // A read that brings nothing has an error: any_stream reports one with no word why as io_error, see
    // io::detail::sound_read.
    throw std::system_error{received.error, "quoll::http::websocket: cannot receive"};
  }
}

// `turn` until the frame goes, given back with it.
async::task<websocket_message> receive_from(std::shared_ptr<state> connection, detail::one_at_a_time turn) {
  auto const held = std::move(turn);

  // What it was in the middle of reading cannot be taken up again.
  if (connection->failure) {
    throw std::system_error{connection->failure, "quoll::http::websocket: receiving has ended"};
  }

  try {
    co_return co_await receive_next(connection);
  } catch (std::system_error const &thrown) {
    connection->failure = thrown.code();
    throw;
  }
}

// Pings the peer once it has read nothing for `interval`, and stops the handler through `gone` once it has read
// nothing for another; sends a close with 1001 when the server drains. Runs until `ends` is stopped or the peer's
// close has come, and only with a timer to wait with.
async::task<> watch_peer(std::shared_ptr<state> connection, std::chrono::nanoseconds interval, std::stop_token drain,
                         std::stop_token ends, std::stop_source &gone) {
  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (!timer) {
    co_return;
  }

  auto const pings = interval > std::chrono::nanoseconds::zero();
  // With no ping, only a drain, a close or the end wakes it.
  auto const period = pings ? interval : std::chrono::nanoseconds{std::chrono::hours{24}};
  auto seen = connection->reads_done.load();
  auto pinged = false;
  auto drained = false;
  // A close has left: the peer's answer is waited for as a pong is.
  auto closing = false;

  while (!ends.stop_requested()) {
    // Before it waits, as a close may have left before this started.
    if (!closing && connection->closing.stop_requested()) {
      closing = true;
      pinged = true;
      seen = connection->reads_done.load();
    }

    std::stop_source wake;
    std::stop_callback const on_end{ends, wake_up{&wake}};
    std::optional<std::stop_callback<wake_up>> on_drain;
    std::optional<std::stop_callback<wake_up>> on_close;

    // Each only until it has woken this once: a stopped token would wake it at once, again and again.
    if (!drained) {
      on_drain.emplace(drain, wake_up{&wake});
    }

    if (!connection->closing.stop_requested()) {
      on_close.emplace(connection->closing.get_token(), wake_up{&wake});
    }

    auto const quiet = co_await async::detail::stop_scope<bool>{timer->wait_for(period), wake.get_token()};

    if (ends.stop_requested() || connection->close_came.load()) {
      co_return;
    }

    if (!drained && drain.stop_requested()) {
      drained = true;

      if (!co_await send_quietly(connection, close_of(*connection, 1001), frame_kind::close, ends)) {
        gone.request_stop();
        co_return;
      }

      continue;
    }

    if (!quiet || !pings) {
      continue;
    }

    auto const now = connection->reads_done.load();

    if (now != seen) {
      seen = now;
      pinged = closing;
      continue;
    }

    // Read nothing since it was pinged, or since a close left: gone, or half-open.
    if (pinged) {
      gone.request_stop();
      co_return;
    }

    if (!co_await send_quietly(connection, frame_of(*connection, websocket_opcode::ping, {}), frame_kind::answer,
                               ends)) {
      gone.request_stop();
      co_return;
    }

    pinged = true;
  }
}

// watch_peer(), which stops the handler too on whatever it throws, as the handler follows `gone` rather than when_all's
// stop. `gone` belongs to serve_websocket's frame, which awaits this at once.
async::task<> keep_alive(std::shared_ptr<state> connection, std::chrono::nanoseconds interval, std::stop_token drain,
                         std::stop_token ends, std::stop_source &gone) {
  try {
    co_await watch_peer(std::move(connection), interval, std::move(drain), std::move(ends), gone);
  } catch (...) {
    gone.request_stop();
    throw;
  }
}

// The handler, under `stop`, which the connection's stop and a peer gone stop. `watch` belongs to serve_websocket's
// frame, which awaits this at once: it ends the keep-alive once the handler has ended, however it ended.
async::task<> run_handler(std::function<async::task<>(websocket)> handler, websocket socket, std::stop_token stop,
                          std::stop_source &watch) {
  async::detail::stop_on_exit const ending{watch};

  co_await async::detail::stop_scope<void>{handler(std::move(socket)), std::move(stop)};
}

// Whether `failure` is a send refused after a close, one that left or the peer's, rather than after a frame cut short.
// Read once the handler is over: what it wrote of the state is behind when_all.
[[nodiscard]] bool refused_after_a_close(state const &connection, std::exception_ptr const &failure) {
  return (connection.sending == detail::sending_state::closed || connection.close_came.load()) &&
         async::detail::code_of(failure) == make_error_code(error::websocket_closed);
}

// The connection, once its handler is over: a close if none left, 1011 if it failed (RFC 6455, 7.4.1), then the
// server's side shut, unless a task left holding a copy still receives, as a stream refuses to shut then.
async::task<> finish(std::shared_ptr<state> connection, bool failed) {
  // A peer gone, or slow: the connection ends all the same.
  static_cast<void>(co_await send_quietly(connection, close_of(*connection, failed ? 1011 : 1000), frame_kind::close));

  if (connection->receiving.exchange(true)) {
    co_return;
  }

  co_await end_sending(connection);
  connection->receiving.store(false);
}

} // namespace

namespace detail {

async::task<> serve_websocket(std::function<async::task<>(websocket)> handler, io::any_stream stream,
                              std::string received, websocket_options options, std::stop_token drain) {
  websocket socket{std::move(stream), websocket_role::server, std::move(received), options};
  auto const connection = websocket_access::state_of(socket);

  // Before any operation: what is sent whole follows it.
  connection->connection_stop = co_await async::current_stop_token();

  // What stops the handler: the connection's stop, or the keep-alive finding the peer gone.
  std::stop_source handler_stop;
  std::stop_callback const forward{connection->connection_stop, wake_up{&handler_stop}};
  std::stop_source watch;
  std::exception_ptr failure;

  try {
    co_await async::when_all(
        run_handler(std::move(handler), std::move(socket), handler_stop.get_token(), watch),
        keep_alive(connection, options.ping_interval, std::move(drain), watch.get_token(), handler_stop));
  } catch (...) {
    failure = std::current_exception();
  }

  // What a stop throws is no failure, the server's or the keep-alive's, nor a send refused after a close, the peer's or
  // a drain's: the connection ends.
  auto const stopped = failure && handler_stop.stop_requested() && async::detail::is_a_stop(failure);
  auto const failed = failure && !stopped && !refused_after_a_close(*connection, failure);

  co_await finish(connection, failed);

  if (failed) {
    std::rethrow_exception(failure);
  }
}

} // namespace detail

websocket::websocket(io::any_stream stream, websocket_role role, std::string received, websocket_options options)
    : state_(std::make_shared<detail::websocket_state>(std::move(stream), role, options)) {
  state_->parser.feed(received);
}

async::task<websocket_message> websocket::receive() {
  detail::one_at_a_time turn{std::shared_ptr<std::atomic<bool>>{state_, &state_->receiving},
                             "quoll::http::websocket: a receive() while another is pending"};

  // Read under the turn, which orders it after what the last receive() did.
  if (state_->close_came.load()) {
    throw std::logic_error{"quoll::http::websocket: a receive() after the close"};
  }

  return receive_from(state_, std::move(turn));
}

async::task<> websocket::send_text(std::string_view text) {
  if (!detail::is_utf8(text)) {
    throw std::invalid_argument{"quoll::http::websocket: text that is not UTF-8"};
  }

  return send_message(state_, frame_of(*state_, websocket_opcode::text, text), frame_kind::message);
}

async::task<> websocket::send_binary(std::string_view data) {
  return send_message(state_, frame_of(*state_, websocket_opcode::binary, data), frame_kind::message);
}

async::task<> websocket::ping(std::string_view data) {
  return send_message(state_, frame_of(*state_, websocket_opcode::ping, data), frame_kind::message);
}

async::task<> websocket::close(std::uint16_t code, std::string_view reason) {
  return send_message(state_, close_of(*state_, code, reason), frame_kind::close);
}

} // namespace quoll::http
