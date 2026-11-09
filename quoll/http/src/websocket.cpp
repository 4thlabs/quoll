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
#include <quoll/async/detail/held_place.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/synchronized.hpp>
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

// Where sending is, under the turn to send. Past open, nothing more may be sent, and who ended it says whose doing a
// send refused then is.
enum class sending_state : std::uint8_t {
  open,
  // A frame cut short leaves the stream in its middle: by what stopped its operation, or by the peer's failure.
  cut_by_handler,
  cut_by_peer,
  // A close has left whole: the answer to the peer's, a drain's or the server's, or the handler's own.
  closed,
  closed_by_handler,
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
  bool close_received = false;
  // Counts the reads that brought something: what tells a peer alive.
  std::atomic<std::uint64_t> reads_done = 0;
  // Counts the writes that took something: what tells the frames ahead of another moving, a peer slowly taking a long
  // message, but not that it is alive, as a stream takes bytes long after a peer has gone.
  std::atomic<std::uint64_t> writes_done = 0;

  // What the peer, or its stream, failed with first: what it throws through a handler is no failure of the handler's.
  async::detail::synchronized<std::error_code> peer_failure;

  // Sending: a frame at a time, in the order they are asked for. What follows is under that turn.
  async::detail::slots writes{1};
  sending_state sending = sending_state::open;
  // The stream's sending side is shut.
  bool sending_shut = false;
  // A close has left, or come: the keep-alive has nothing more to watch, as the close handshake is finish()'s.
  std::atomic<bool> closing = false;

  // How long what the server sends on its own, a keep-alive frame, an answer to the peer, its last close, may take to
  // leave, and how long it waits for the peer's close: `ping_interval`, or 30 seconds where it is zero, as a peer that
  // no longer reads must not hold a connection whose application has stopped watching it. Only with a timer.
  std::chrono::nanoseconds wait_limit;

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

[[nodiscard]] std::error_code peer_failure_of(state &connection) {
  return connection.peer_failure.with([](std::error_code const &first) { return first; });
}

// What the peer, or its stream, made an operation throw, told apart from what a handler throws on its own by its type,
// whatever its code, as the server's body_not_read is: a handler that lets it through has not failed.
class of_the_peer : public std::system_error {
public:
  using std::system_error::system_error;
};

// Throws `code`: as of_the_peer if the peer is the cause, a stop once it had failed included, rather than the
// handler's own stop or its own close.
[[noreturn]] void fail(state &connection, std::error_code code, char const *what) {
  auto const stopped = code == std::errc::operation_canceled;
  auto const handlers_doing =
      code == error::websocket_closed && (connection.sending == detail::sending_state::closed_by_handler ||
                                          connection.sending == detail::sending_state::cut_by_handler);

  if ((!stopped && !handlers_doing) || (stopped && peer_failure_of(connection))) {
    throw of_the_peer{code, what};
  }

  throw std::system_error{code, what};
}

// What a frame is to what sends it once nothing more may be sent: a message refused, an answer or a close dropped. The
// handler's own close says so as it leaves.
enum class frame_kind : std::uint8_t { message, answer, close, own_close };

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

// `frame` written, the turn to send held. A loop of its own rather than io::write(): each piece that leaves tells the
// peer alive, as one slowly taking a long message is.
async::task<> write_frame(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  if (connection->sending != detail::sending_state::open) {
    if (kind == frame_kind::message) {
      fail(*connection, make_error_code(error::websocket_closed), "quoll::http::websocket: cannot send");
    }

    co_return;
  }

  auto rest = std::as_bytes(std::span{frame});

  while (!rest.empty()) {
    auto const sent = co_await connection->stream.write_some(rest);

    // A stream that takes nothing, or more than it was given, without saying why, breaks its contract: see io::write.
    if (sent.error || sent.transferred == 0 || sent.transferred > rest.size()) {
      auto const failure = sent.error ? sent.error : std::make_error_code(std::errc::io_error);

      record_peer_failure(*connection, failure);

      // A frame of which nothing left leaves sending open, unless the peer has failed.
      if (peer_failure_of(*connection)) {
        connection->sending = detail::sending_state::cut_by_peer;
      }

      fail(*connection, failure, "quoll::http::websocket: cannot send");
    }

    // Until it has left whole: what stops its operation midway cuts it, unless the peer is found to have failed.
    connection->sending = detail::sending_state::cut_by_handler;
    connection->writes_done.fetch_add(1);
    rest = rest.subspan(sent.transferred);
  }

  connection->sending = kind == frame_kind::own_close ? detail::sending_state::closed_by_handler
                        : kind == frame_kind::close   ? detail::sending_state::closed
                                                      : detail::sending_state::open;
  connection->closing.store(connection->sending != detail::sending_state::open);
}

// write_frame(), its turn held, whole rather than cut by the stop of the operation it serves, which a peer would read
// as a broken stream, but within the connection's wait_limit where there is a timer: what does not take it is a peer
// gone.
async::task<> write_whole(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  try {
    co_await async::detail::stop_scope<void>{
        async::detail::with_optional_timeout(connection->wait_limit, write_frame(connection, std::move(frame), kind)),
        std::stop_token{}};
  } catch (std::system_error const &thrown) {
    record_peer_failure(*connection, thrown.code());

    if (connection->sending == detail::sending_state::cut_by_handler) {
      connection->sending = detail::sending_state::cut_by_peer;
    }

    fail(*connection, thrown.code(), "quoll::http::websocket: the peer takes nothing");
  }
}

// What answers the peer, a pong or a close: its turn waited for under the stop of the operation it serves, as nothing
// is cut then, then written whole.
async::task<> send_answer(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  async::detail::held_place turn;

  co_await turn.take(connection->writes);
  co_await write_whole(connection, std::move(frame), kind);
}

// `frame` sent, once those asked before it have left.
async::task<> send_frame(std::shared_ptr<state> connection, std::string frame, frame_kind kind) {
  async::detail::held_place turn;

  co_await turn.take(connection->writes);
  co_await write_frame(connection, std::move(frame), kind);
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
    async::detail::held_place turn;

    co_await turn.take(connection->writes);

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
        co_await send_answer(connection,
                             format_websocket_close(close_code_of(parsed.error()), {}, mask_of(*connection)),
                             frame_kind::close);
      } catch (std::system_error const &) {
      }

      fail(*connection, parsed.error(), "quoll::http::websocket: the peer broke the protocol");
    }

    if (auto &message = *parsed) {
      switch (message->opcode) {
      case websocket_opcode::ping:
        co_await send_answer(connection,
                             format_websocket_frame(websocket_opcode::pong, message->data, true, mask_of(*connection)),
                             frame_kind::answer);
        continue;
      case websocket_opcode::pong:
        continue;
      case websocket_opcode::close:
        connection->close_received = true;
        connection->closing.store(true);

        // A peer that closed its stream after its close, as some do, has had all it needs.
        try {
          co_await send_answer(connection, close_answering(*message, mask_of(*connection)), frame_kind::close);
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
      connection->reads_done.fetch_add(1);
      connection->parser.feed(std::string_view{chunk.data(), received.transferred});
      continue;
    }

    // A read that brings nothing has an error: any_stream reports one with no word why as io_error, see
    // io::detail::sound_read.
    record_peer_failure(*connection, received.error);
    fail(*connection, received.error, "quoll::http::websocket: cannot receive");
  }
}

// `turn` until the frame goes, given back with it.
async::task<websocket_message> receive_from(std::shared_ptr<state> connection, detail::one_at_a_time turn) {
  auto const held = std::move(turn);

  // What a stop left cannot be taken up again, as the stream may be in the middle of a frame: the handler's doing.
  if (connection->failure == std::errc::operation_canceled) {
    throw std::system_error{make_error_code(error::websocket_closed), "quoll::http::websocket: receiving was stopped"};
  }

  if (connection->failure) {
    fail(*connection, connection->failure, "quoll::http::websocket: receiving has failed");
  }

  try {
    co_return co_await receive_next(connection);
  } catch (std::system_error const &thrown) {
    // Whatever it was in the middle of reading cannot be taken up again.
    connection->failure = thrown.code();
    throw;
  }
}

// Whether `failure`, thrown through a handler, is no failure of its own: what the peer made an operation throw, or the
// stop of a handler whose peer had failed, the keep-alive's doing. Read once the handler and the keep-alive are over:
// what they wrote of the state is behind when_all.
[[nodiscard]] bool comes_of_the_peer(state &connection, std::exception_ptr const &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (of_the_peer const &) {
    return true;
  } catch (std::system_error const &thrown) {
    return thrown.code() == std::errc::operation_canceled && peer_failure_of(connection);
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

// What a stop asks of a wait: to end it.
struct wake_up {
  std::stop_source *wake;

  void operator()() const noexcept { wake->request_stop(); }
};

// `frame` sent for the keep-alive. Its turn is waited for as long as what is ahead of it moves, as a peer slowly taking
// a long message is no peer gone, and not once `ends` is stopped, the handler over. Then it goes whole: see
// write_whole().
async::task<> send_in_time(std::shared_ptr<state> connection, std::string frame, frame_kind kind,
                           std::stop_token ends) {
  async::detail::held_place turn;
  auto seen = connection->writes_done.load();

  for (;;) {
    std::error_code waited;

    try {
      co_await async::detail::stop_scope<void>{
          async::with_timeout(connection->wait_limit, turn.take(connection->writes)), ends};
    } catch (std::system_error const &thrown) {
      waited = thrown.code();
    }

    if (ends.stop_requested()) {
      co_return;
    }

    if (!waited) {
      break;
    }

    auto const now = connection->writes_done.load();

    if (waited != std::errc::timed_out || now == seen) {
      record_peer_failure(*connection, waited);
      fail(*connection, waited, "quoll::http::websocket: the peer takes nothing");
    }

    seen = now;
  }

  co_await write_whole(connection, std::move(frame), kind);
}

// Pings the peer once it has been quiet for `interval`, and fails once it has stayed so for another, which stops the
// handler beside this; sends a close with 1001 when the server drains. Runs until `ends` is stopped, and only with a
// timer to wait with.
async::task<> keep_alive(std::shared_ptr<state> connection, std::chrono::nanoseconds interval, std::stop_token drain,
                         std::stop_token ends) {
  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (!timer) {
    co_return;
  }

  auto const pings = interval > std::chrono::nanoseconds::zero();
  // With no ping, only a drain or the end wakes it.
  auto const period = pings ? interval : std::chrono::nanoseconds{std::chrono::hours{24}};
  auto seen = connection->reads_done.load();
  auto pinged = false;
  auto drained = false;

  while (!ends.stop_requested()) {
    std::stop_source wake;
    std::stop_callback const on_end{ends, wake_up{&wake}};
    std::optional<std::stop_callback<wake_up>> on_drain;

    if (!drained) {
      on_drain.emplace(drain, wake_up{&wake});
    }

    auto const quiet = co_await async::detail::stop_scope<bool>{timer->wait_for(period), wake.get_token()};

    if (ends.stop_requested() || connection->closing.load()) {
      co_return;
    }

    if (!drained && drain.stop_requested()) {
      drained = true;

      co_await send_in_time(connection, format_websocket_close(1001, {}, mask_of(*connection)), frame_kind::close,
                            ends);
      continue;
    }

    if (!quiet || !pings) {
      continue;
    }

    auto const now = connection->reads_done.load();

    if (now != seen) {
      seen = now;
      pinged = false;
      continue;
    }

    if (pinged) {
      auto const silent = std::make_error_code(std::errc::timed_out);

      record_peer_failure(*connection, silent);
      fail(*connection, silent, "quoll::http::websocket: the peer has gone quiet");
    }

    co_await send_in_time(connection, format_websocket_frame(websocket_opcode::ping, {}, true, mask_of(*connection)),
                          frame_kind::answer, ends);
    pinged = true;
  }
}

// The peer's messages, dropped, up to its close: what the handler left unread.
async::task<> read_to_close(std::shared_ptr<state> connection) {
  while ((co_await receive_next(connection)).opcode != websocket_opcode::close) {
  }
}

// The connection, once its handler is over, its turn to receive held: a close if it sent none, 1011 if `failed` (RFC
// 6455, 7.4.1), then what the peer sends, read up to its close if `read`.
async::task<> close_and_read(std::shared_ptr<state> connection, bool failed, bool read) {
  co_await send_frame(connection, format_websocket_close(failed ? 1011 : 1000, {}, mask_of(*connection)),
                      frame_kind::close);

  auto const close_left = connection->sending == detail::sending_state::closed ||
                          connection->sending == detail::sending_state::closed_by_handler;

  // A close that could not leave, behind a frame cut short, gets no answer to wait for.
  if (!read || !close_left || connection->close_received || connection->failure) {
    co_return;
  }

  try {
    co_await read_to_close(connection);
  } catch (std::system_error const &) {
    // The peer gone meanwhile.
  }
}

// Whether receiving ended on a peer that broke the protocol, which was sent its close. Under the turn to receive.
[[nodiscard]] bool broke_the_protocol(state const &connection) noexcept {
  return connection.failure && connection.failure.category() == http_category();
}

// What the peer still sends, dropped until it ends, its turn to receive held: what follows a broken protocol, where no
// close can be read.
async::task<> drop_what_comes(std::shared_ptr<state> connection) {
  std::array<char, 4096> chunk;

  for (;;) {
    auto const received = co_await connection->stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (received.error || received.transferred == 0) {
      co_return;
    }
  }
}

// The connection, once its handler is over: close_and_read() within its wait_limit, then the stream's sending side
// shut, so that it ends behind the close rather than with a reset that may lose it. A peer that broke the protocol has
// had its close: its side is shut at once, then what it still sends dropped within wait_limit. The waits need a timer.
// A task left holding a copy, still receiving, keeps the turn to receive: only the close goes then, as a stream refuses
// to shut while a read is pending, and that task ends the connection.
async::task<> finish(std::shared_ptr<state> connection, bool failed) {
  auto const timed = (co_await async::current_executor()).query<async::timer_ref>().has_value();
  auto const limit = connection->wait_limit;
  auto const turn_free = !connection->receiving.exchange(true);
  auto const broken = turn_free && broke_the_protocol(*connection);

  try {
    // Its close went as it was found, to a peer that broke the protocol.
    if (!broken) {
      co_await async::detail::with_optional_timeout(limit, close_and_read(connection, failed, turn_free && timed));
    }
  } catch (std::system_error const &) {
    // The peer slow to answer, or gone, or a stop: the connection ends all the same.
  }

  if (!turn_free) {
    co_return;
  }

  try {
    co_await async::detail::with_optional_timeout(limit, end_sending(connection));

    if (broken && timed) {
      co_await async::with_timeout(limit, drop_what_comes(connection));
    }
  } catch (std::system_error const &) {
    // Nor does a stream that does not take it, or a peer that goes on sending, hold it.
  }

  connection->receiving.store(false);
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

  auto const peer_caused = failure && comes_of_the_peer(*connection, failure);
  auto const peer_failure = peer_failure_of(*connection);

  // A peer gone, or silent, is owed nothing more, and would only hold the connection longer; one that broke the
  // protocol is still there.
  if (!peer_failure || peer_failure.category() == http_category()) {
    co_await finish(connection, failure && !peer_caused);
  }

  if (failure && !peer_caused) {
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
  if (state_->close_received) {
    throw std::logic_error{"quoll::http::websocket: a receive() after the close"};
  }

  return receive_from(state_, std::move(turn));
}

async::task<> websocket::send_text(std::string_view text) {
  if (!detail::is_utf8(text)) {
    throw std::invalid_argument{"quoll::http::websocket: text that is not UTF-8"};
  }

  return send_frame(state_, format_websocket_frame(websocket_opcode::text, text, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::send_binary(std::string_view data) {
  return send_frame(state_, format_websocket_frame(websocket_opcode::binary, data, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::ping(std::string_view data) {
  return send_frame(state_, format_websocket_frame(websocket_opcode::ping, data, true, mask_of(*state_)),
                    frame_kind::message);
}

async::task<> websocket::close(std::uint16_t code, std::string_view reason) {
  return send_frame(state_, format_websocket_close(code, reason, mask_of(*state_)), frame_kind::own_close);
}

} // namespace quoll::http
