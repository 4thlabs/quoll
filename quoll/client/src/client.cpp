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

#include <quoll/client/client.hpp>
#include <quoll/io/network_connector.hpp>

#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/held_place.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_on_exit.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/client/detail/max_interim_responses.hpp>
#include <quoll/client/detail/peer_has_gone.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/streamed_head.hpp>
#include <quoll/http/detail/streamed_wire.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/io/write.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace quoll::http::detail {

namespace {

// The clock a client's connections are idle on: that of the executor's timer, virtual in a test, or steady_clock's
// without a timer, as asio's timer is. The executors a client is used from share it, as Quoll's runtimes do: a timer
// has an origin of its own, and nothing tells two of them apart.
[[nodiscard]] std::chrono::nanoseconds idle_clock(async::executor_ref executor) {
  if (auto const timer = executor.query<async::timer_ref>()) {
    return timer->now();
  }

  return std::chrono::steady_clock::now().time_since_epoch();
}

// The idle connections of a client, between two requests. Thread safety: take() and give_back() may be called
// from any thread, concurrently. The lock serves the requests of one client sent from several threads (a thread
// pool); with a single thread, as under WASM, it is never contended, and std::mutex needs no thread support there.
class connection_pool {
public:
  // A connection from the pool, with when it went there, in order, and when its last request was sent, on the clock.
  struct idle_connection {
    io::any_stream connection;
    std::uint64_t given_back;
    std::chrono::nanoseconds sent_at;
  };

  explicit connection_pool(std::size_t capacity) : capacity_(capacity) {}

  // How recently a connection's last request must have been sent for it to be taken: less than `within` before `now`,
  // on idle_clock().
  struct recency {
    std::chrono::nanoseconds now;
    std::chrono::nanoseconds within;
  };

  // The connection used last, the least likely to have been closed by the server for being idle; given `recent`, the
  // one given back last of those whose last request went that recently, which a response read slowly may have given
  // back after one sent later. One sent after `now`, on an executor with another clock or meanwhile on another thread,
  // is not taken, as nothing tells how long it has been idle.
  [[nodiscard]] std::optional<idle_connection> take(std::optional<recency> recent = std::nullopt) {
    return state_.with([recent](state &pool) -> std::optional<idle_connection> {
      auto const recent_enough = [recent](idle_connection const &kept) {
        if (!recent) {
          return true;
        }

        auto const idle_for = recent->now - kept.sent_at;

        return idle_for >= std::chrono::nanoseconds::zero() && idle_for < recent->within;
      };
      auto const found = std::ranges::find_if(pool.idle.rbegin(), pool.idle.rend(), recent_enough);

      if (found == pool.idle.rend()) {
        return std::nullopt;
      }

      auto const at = std::prev(found.base());
      auto kept = std::move(*at);

      pool.idle.erase(at);
      return kept;
    });
  }

  // Keeps `connection` for a next request, whose last one was sent at `sent_at` on idle_clock(): the server cannot have
  // seen it idle for longer, as it went idle once it had answered, however slowly the answer was read. A full pool
  // closes its oldest, past the lock, rather than this one, the least likely to be closed by the server, and the only
  // one a POST may take.
  void give_back(io::any_stream connection, std::chrono::nanoseconds sent_at) {
    if (capacity_ == 0) {
      return;
    }

    auto const oldest = state_.with([&](state &pool) {
      std::optional<idle_connection> dropped;

      if (pool.idle.size() >= capacity_) {
        dropped.emplace(std::move(pool.idle.front()));
        pool.idle.erase(pool.idle.begin());
      }

      pool.idle.push_back({std::move(connection), ++pool.given, sent_at});
      return dropped;
    });
  }

  // Closes the idle connections past `count`, the oldest first, past the lock.
  void keep_at_most(std::size_t count) {
    auto const closed = state_.with([count](state &pool) {
      std::vector<idle_connection> past_it;

      if (pool.idle.size() > count) {
        auto const end = pool.idle.begin() + static_cast<std::ptrdiff_t>(pool.idle.size() - count);

        past_it.assign(std::make_move_iterator(pool.idle.begin()), std::make_move_iterator(end));
        pool.idle.erase(pool.idle.begin(), end);
      }

      return past_it;
    });
  }

  // Closes the connections idle for longer than one that turned out closed, past the lock. Those given back since it
  // was taken have just been of use.
  void close_older_than(std::uint64_t given_back) {
    auto const closed = state_.with([given_back](state &pool) {
      std::vector<idle_connection> older;
      auto const end = std::ranges::find_if(
          pool.idle, [given_back](idle_connection const &kept) { return kept.given_back > given_back; });

      older.assign(std::make_move_iterator(pool.idle.begin()), std::make_move_iterator(end));
      pool.idle.erase(pool.idle.begin(), end);
      return older;
    });
  }

private:
  struct state {
    // The oldest first.
    std::vector<idle_connection> idle;
    std::uint64_t given = 0;
  };

  async::detail::synchronized<state> state_;
  std::size_t capacity_;
};

// A request serialized by send(), before its task runs, with what the exchange needs to know about it.
struct prepared_request {
  std::string wire;
  http::verb method = http::verb::get;
  // The request lets its connection carry another one (HTTP/1.1, no `Connection: close`).
  bool keep_alive = false;
  // A body in pieces: `wire` is then its head.
  http::body_writer writer{};
  http::detail::streamed_framing framing{};
  http::fields trailers{};
};

// Methods whose request may be sent twice: the server may have received the first copy before closing. Listed rather
// than excluded, so that a method added to http::verb is not sent twice by default.
[[nodiscard]] constexpr bool idempotent(http::verb method) noexcept {
  switch (method) {
  case http::verb::get:
  case http::verb::head:
  case http::verb::put:
  case http::verb::delete_:
  case http::verb::options:
  case http::verb::trace:
    return true;
  default:
    return false;
  }
}

// A write that fails for the connection's sake, however a socket or TLS says so, leaves the server's answer a chance,
// with something to end the wait for it if the connection stays open. What this side decided ends things at once.
[[nodiscard]] bool answer_may_still_come(std::error_code error) noexcept {
  return error != std::errc::timed_out && error != std::errc::operation_canceled && error != std::errc::message_size;
}

// The body of a request going out through its writer, while what reads the response goes on beside it: a server may
// answer before the end, and one that does not read while it writes would otherwise hold both up. Each write has
// `timeout`, and the response too, once the body has left. An answer stops what is still writing. What a server closing
// does to a write is no failure yet: its answer may be on its way. Thread safety: stop() from what
// reads, and failure() and complete() once the when_all around them has ended, as what reads and what writes may run on
// two threads.
class upload {
public:
  upload(io::any_stream &connection QUOLL_LIFETIMEBOUND, prepared_request const &request QUOLL_LIFETIMEBOUND,
         std::chrono::nanoseconds timeout)
      : connection_(&connection), request_(&request), wire_(request.wire, request.framing), timeout_(timeout) {}

  // Ends once the answer has come, or throws what ends it without one.
  [[nodiscard]] async::task<> run() QUOLL_LIFETIMEBOUND {
    auto const outer = co_await async::current_stop_token();
    std::stop_callback const follow{outer, [this] { stop_.request_stop(); }};

    try {
      co_await async::detail::stop_scope<void>{request_->writer(http::body_stream{*this}), stop_.get_token()};
      co_await async::detail::stop_scope<void>{finish(), stop_.get_token()};
    } catch (std::system_error const &thrown) {
      // The server closing is no failure yet, unlike what the writer makes of it.
      if (!stop_.stop_requested() && !(thrown.code() == failure_ && answer_may_still_come(failure_))) {
        throw;
      }
    } catch (...) {
      if (!stop_.stop_requested()) {
        throw;
      }
    }

    auto const executor = co_await async::current_executor();

    // Nothing to end what reads the response, on a connection that may stay open: it would wait for ever.
    auto const timer_to_wait_on = timeout_ != std::chrono::nanoseconds::zero() && executor.query<async::timer_ref>();

    if (failure_ && !stop_.stop_requested() && !peer_has_gone(failure_) && !timer_to_wait_on) {
      throw std::system_error{failure_, "quoll::http::client: cannot send the body"};
    }

    // With a timer to wait on: what reads the response has its own end otherwise.
    if (stop_.stop_requested() || !timer_to_wait_on) {
      co_return;
    }

    try {
      co_await async::detail::stop_scope<void>{async::sleep_for(timeout_), stop_.get_token()};
    } catch (std::system_error const &) {
      // A stop ends the wait; a timer that fails leaves nothing else to limit it.
      if (!stop_.stop_requested()) {
        throw;
      }
    }

    // An answer in the same instant as the timer: it stands.
    if (stop_.stop_requested()) {
      co_return;
    }

    // No answer came to what could not leave: what went wrong is that.
    if (failure_) {
      throw std::system_error{failure_, "quoll::http::client: cannot send the body"};
    }

    throw std::system_error{std::make_error_code(std::errc::timed_out),
                            "quoll::http::client: no response to the request"};
  }

  // What the writer writes through: see http::body_stream.
  [[nodiscard]] async::task<> write(std::string_view piece QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    if (failure_) {
      throw std::system_error{failure_, "quoll::http::client: the body cannot go on"};
    }

    if (stop_.stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled),
                              "quoll::http::client: the body was answered before its end"};
    }

    if (wire_.past_its_length(piece)) {
      failure_ = std::make_error_code(std::errc::message_size);
      throw std::length_error{"quoll::http::client: a body_writer wrote more than its Content-Length"};
    }

    if (auto const bytes = wire_.frame(piece); !bytes.empty()) {
      co_await send(bytes);
    }
  }

  [[nodiscard]] std::stop_source &stop() noexcept QUOLL_LIFETIMEBOUND { return stop_; }

  // What stopped the body: once run() has ended.
  [[nodiscard]] std::error_code failure() const noexcept { return failure_; }

  // All of the request has left: once run() has ended.
  [[nodiscard]] bool complete() const noexcept { return complete_; }

private:
  [[nodiscard]] async::task<> finish() QUOLL_LIFETIMEBOUND {
    // A writer may have made nothing of it.
    if (failure_) {
      throw std::system_error{failure_, "quoll::http::client: cannot send the body"};
    }

    if (stop_.stop_requested()) {
      co_return;
    }

    if (wire_.short_of_its_length()) {
      throw std::length_error{"quoll::http::client: a body_writer wrote less than its Content-Length"};
    }

    if (auto const bytes = wire_.end(request_->trailers); !bytes.empty()) {
      co_await send(bytes);
    }

    complete_ = true;
  }

  // Thrown rather than returned: a write a timeout cancelled is then that timeout's timed_out.
  [[nodiscard]] async::task<> write_bytes(std::string_view bytes QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    auto const sent = co_await io::write(*connection_, std::as_bytes(std::span{bytes}));

    if (sent.error) {
      throw std::system_error{sent.error, "quoll::http::client: cannot send the body"};
    }
  }

  [[nodiscard]] async::task<> send(std::string_view bytes QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    try {
      co_await async::detail::with_optional_timeout(timeout_, write_bytes(bytes));
    } catch (std::system_error const &thrown) {
      failure_ = thrown.code();
      throw;
    }
  }

  io::any_stream *connection_;
  prepared_request const *request_;
  http::detail::streamed_wire wire_;
  std::chrono::nanoseconds timeout_;
  std::stop_source stop_;
  std::error_code failure_;
  bool complete_ = false;
};

// What reads the response beside an upload: its end, however it ends, stops what is still writing.
template <class T>
[[nodiscard]] async::task<T> until_answered(upload &sending QUOLL_LIFETIMEBOUND, async::task<T> reading) {
  async::detail::stop_on_exit const answered{sending.stop()};

  co_return co_await std::move(reading);
}

// For the handler of what failed around an upload, once both sides have ended. The same as for a whole body: gone with
// no answer, its request could not leave. Not for a write that the read ending has stopped: the read says what
// happened.
[[noreturn]] void throw_what_ended(upload const &sending, std::system_error const &thrown) {
  if (sending.failure() && answer_may_still_come(sending.failure()) && peer_has_gone(thrown.code())) {
    throw std::system_error{sending.failure(), "quoll::http::client: cannot send the body"};
  }

  throw;
}

// On which connection a request goes, and so when it goes again on a new one.
enum class attempt : std::uint8_t {
  // A new connection: a response or an exception.
  fresh,
  // An idle connection, for a request not sent twice: again only if none of it could be written, as the server then did
  // not read any of it, or if it is answered 408 with `Connection: close`, as the server then reads none of it. Another
  // 408 there is its response, and the connection is not kept, as the server may answer the request after it.
  idle_once,
  // An idle connection, for an idempotent request: again if the connection turns out closed before any response, or
  // answers 408 first, sent by the server before it closed the idle connection.
  idle_again,
};

// On an idle connection whose write failed as the server had gone, whether the request goes again on a new one: an
// idempotent one does, and so does any none of which was written, which the server never read, as what it sent there
// before closing, a 408 or a 503, answers none of it.
[[nodiscard]] constexpr bool goes_again_unsent(attempt on, std::size_t transferred) noexcept {
  return on == attempt::idle_again || (on == attempt::idle_once && transferred == 0);
}

// On an idle connection answered 408 before any interim response, whether the request goes again on a new one: an
// idempotent one does, as the server sent it as it closed the idle connection, and so does any other when the 408
// closes the connection, as the server then reads no request on it (RFC 9110, 15.5.9).
[[nodiscard]] constexpr bool goes_again_after_408(attempt on, bool closes) noexcept {
  return on == attempt::idle_again || (on == attempt::idle_once && closes);
}

// Whether a connection may carry another request after a response of `status`: not one that answered 408 as it was
// idle, to a request not sent twice, as the server may answer that request on it after its 408.
[[nodiscard]] constexpr bool may_go_on_after(attempt on, http::status status) noexcept {
  return on != attempt::idle_once || status != http::status::request_timeout;
}

// Sends `request` on `connection` and reads its final response, skipping the interim 1xx ones; the connection goes
// back to `pool` if it can carry another request. On an idle connection, `on` says when there is no response, and the
// caller sends the request again on a new connection; otherwise, a response or an exception.
[[nodiscard]] async::task<std::optional<http::response>>
exchange(connection_pool &pool QUOLL_LIFETIMEBOUND, http::parse_limits limits,
         io::any_stream &connection QUOLL_LIFETIMEBOUND, prepared_request const &request QUOLL_LIFETIMEBOUND,
         attempt on, bool *reusable QUOLL_LIFETIMEBOUND = nullptr) {
  std::error_code unsent;
  // Only for a connection that may go back to the pool.
  std::chrono::nanoseconds sent_at{};

  if (reusable == nullptr && request.keep_alive) {
    sent_at = idle_clock(co_await async::current_executor());
  }

  // Its request is on its way, and its connection stays with what sends it.
  if (reusable == nullptr) {
    auto const sent = co_await io::write(connection, std::as_bytes(std::span{request.wire}));

    if (sent.error) {
      if (!peer_has_gone(sent.error)) {
        throw std::system_error{sent.error, "quoll::http::client: cannot send the request"};
      }

      // On an idle connection, an idempotent request goes again, and so does any of which nothing was written: the
      // server never read it, and what it sent there, as a 408 before it closed, answers no request of it.
      if (goes_again_unsent(on, sent.transferred)) {
        co_return std::nullopt;
      }

      // A server may answer a request it will not read, a 413 or a 503, and go: its answer is there to read.
      unsent = sent.error;
    }
  }

  std::string buffer;
  http::response_parser parser{limits};
  auto interim = 0;

  parser.set_request_method(request.method);

  for (;;) {
    if (auto const error = co_await http::read(connection, buffer, parser)) {
      // Gone with no answer: an idempotent request goes again.
      if (interim == 0 && peer_has_gone(error) && on == attempt::idle_again) {
        co_return std::nullopt;
      }

      // Gone with no answer. Any other failure is what it says, a timeout's or a bad message's.
      if (unsent && peer_has_gone(error)) {
        throw std::system_error{unsent, "quoll::http::client: cannot send the request"};
      }

      throw std::system_error{error, "quoll::http::client: cannot read the response"};
    }

    auto const keep_alive = parser.keep_alive();
    auto const upgrade = parser.upgrade();
    auto response = parser.release();

    if (http::is_informational(response.status) && !upgrade) {
      if (++interim > max_interim_responses) {
        throw std::system_error{http::error::bad_message, "quoll::http::client: too many interim responses"};
      }

      continue;
    }

    auto const timed_out = response.status == http::status::request_timeout;

    if (interim == 0 && timed_out && goes_again_after_408(on, http::detail::closes_connection(response.fields))) {
      co_return std::nullopt;
    }

    // Bytes past the response would be read as the next one: a connection carrying them is not reused, nor one that
    // answered 408 as it was idle, on which the server may answer the request after it.
    auto const can_go_on = request.keep_alive && keep_alive && !upgrade && buffer.empty() && !unsent &&
                           may_go_on_after(on, response.status);

    if (reusable != nullptr) {
      *reusable = can_go_on;
    } else if (can_go_on) {
      pool.give_back(std::move(connection), sent_at);
    }

    co_return response;
  }
}

// exchange() for a request with a body in pieces: on a connection of its own, as a writer runs once and nothing can be
// sent again.
[[nodiscard]] async::task<http::response> exchange_uploading(connection_pool &pool QUOLL_LIFETIMEBOUND,
                                                             http::parse_limits limits, io::any_stream connection,
                                                             prepared_request const &request QUOLL_LIFETIMEBOUND,
                                                             std::chrono::nanoseconds timeout) {
  upload sending{connection, request, timeout};
  auto const sent_at = idle_clock(co_await async::current_executor());
  auto reusable = false;
  std::optional<http::response> response;

  try {
    auto [uploaded, answer] = co_await async::when_all(
        sending.run(), until_answered(sending, exchange(pool, limits, connection, request, attempt::fresh, &reusable)));

    response = std::move(answer);
  } catch (std::system_error const &thrown) {
    throw_what_ended(sending, thrown);
  }

  assert(response && "exchange() on a new connection produced no response");

  // Once all is over, for its last write to have ended, as what comes next on it would read what a cut one left.
  if (reusable && sending.complete()) {
    pool.give_back(std::move(connection), sent_at);
  }

  co_return std::move(*response);
}

// What the copies of a client share. Thread safety: what does not change, and what holds its own lock.
class client_core {
public:
  client_core(connector_handle to, std::string name, std::uint16_t number, bool tls, std::string field,
              std::string path, client_options const &given)
      : connector(std::move(to)), host(std::move(name)), port(number), secure(tls), host_field(std::move(field)),
        prefix(std::move(path)), options(given), pool(given.max_idle_connections) {
    if (given.max_connections > 0) {
      places.emplace(given.max_connections);
    }
  }

  [[nodiscard]] async::task<io::any_stream> open() const {
    return connector.open(connector.connector.get(), host, port, secure, options.tls);
  }

  connector_handle connector;
  std::string host;
  std::uint16_t port;
  bool secure;
  std::string host_field;
  std::string prefix;
  client_options options;
  connection_pool pool;
  // Requests on a connection at once, when options.max_connections limits them.
  std::optional<async::detail::slots> places;
};

// A stop already asked for: no connection is taken or opened, as what it would be awaited under completes without a
// word on it, and it would close having carried nothing.
void throw_if_stopped(std::stop_token const &token) {
  if (token.stop_requested()) {
    throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::http::client: stopped"};
  }
}

// A new connection is about to open: what is idle past the limit makes room for it.
void make_room(client_core &shared) {
  if (shared.places) {
    shared.pool.keep_at_most(shared.places->available());
  }
}

// An idle connection a request may take, and how it goes on it.
struct idle_attempt {
  connection_pool::idle_connection idle;
  attempt on;
};

// The idle connection a request takes, as Go's and Boost.Burl's clients do, but for one whose body comes in pieces,
// which has its own, as a writer runs once. One the server closed meanwhile shows only once the request is on it: an
// idempotent request takes any, and goes again on a new one; another, which is not sent twice, takes only one that went
// idle recently, below the servers' idle timeouts, so that it rarely meets one closed.
[[nodiscard]] std::optional<idle_attempt> take_idle(client_core &shared, prepared_request const &request,
                                                    async::executor_ref executor) {
  if (request.writer) {
    return std::nullopt;
  }

  if (idempotent(request.method)) {
    auto idle = shared.pool.take();

    return idle ? std::optional<idle_attempt>{{std::move(*idle), attempt::idle_again}} : std::nullopt;
  }

  auto const within = shared.options.max_idle_for_non_idempotent;
  auto idle =
      within > std::chrono::nanoseconds::zero() ? shared.pool.take({{idle_clock(executor), within}}) : std::nullopt;

  return idle ? std::optional<idle_attempt>{{std::move(*idle), attempt::idle_once}} : std::nullopt;
}

// The task holds the shared state, not the client.
[[nodiscard]] async::task<http::response> send_prepared(std::shared_ptr<client_core> shared, prepared_request request) {
  async::detail::held_place place;

  if (shared->places) {
    co_await async::detail::with_optional_timeout(shared->options.connect_timeout, place.take(*shared->places));
  }

  throw_if_stopped(co_await async::current_stop_token());

  auto const executor = co_await async::current_executor();

  if (auto kept = take_idle(*shared, request, executor)) {
    auto const given_back = kept->idle.given_back;
    std::optional<http::response> response;

    try {
      response = co_await async::detail::with_optional_timeout(
          shared->options.response_timeout,
          exchange(shared->pool, shared->options.limits, kept->idle.connection, request, kept->on));
    } catch (std::system_error const &thrown) {
      // Closed by the server after some idle time: the connections idle for longer are likely closed as well.
      if (peer_has_gone(thrown.code())) {
        shared->pool.close_older_than(given_back);
      }

      throw;
    }

    // A 408 as the response of a request not sent twice: the same, sent as the server closed the idle connection.
    if (!response || response->status == http::status::request_timeout) {
      shared->pool.close_older_than(given_back);
    }

    if (response) {
      co_return std::move(*response);
    }
  }

  make_room(*shared);

  auto connection = co_await async::detail::with_optional_timeout(shared->options.connect_timeout, shared->open());

  if (request.writer) {
    co_return co_await exchange_uploading(shared->pool, shared->options.limits, std::move(connection), request,
                                          shared->options.response_timeout);
  }

  auto response = co_await async::detail::with_optional_timeout(
      shared->options.response_timeout,
      exchange(shared->pool, shared->options.limits, connection, request, attempt::fresh));

  // On a new connection, exchange() produces a response or throws.
  assert(response && "exchange() on a new connection produced no response");
  co_return std::move(*response);
}

// The request as it goes on the wire. Throws std::invalid_argument for what http::serialize() rejects.
[[nodiscard]] prepared_request prepare(client_core const &shared, http::request &request) {
  auto const keep_alive = request.version == http::version::http_1_1 &&
                          !http::detail::closes_connection(request.fields) && shared.options.max_idle_connections > 0;

  prepared_request prepared{.method = request.method, .keep_alive = keep_alive};

  if (request.body_writer) {
    prepared.framing = http::detail::serialize_streamed_head(request, prepared.wire);
    // What it captures may be large, and nothing follows a redirect with it.
    prepared.writer = std::move(request.body_writer);
    prepared.trailers = std::move(request.trailers);
  } else {
    http::serialize(request, prepared.wire);
  }

  return prepared;
}

// The statuses that ask for the same thing elsewhere: not 300, a choice for a person, nor 304, which has no elsewhere.
[[nodiscard]] constexpr bool redirects(http::status status) noexcept {
  switch (status) {
  case http::status::moved_permanently:
  case http::status::found:
  case http::status::see_other:
  case http::status::temporary_redirect:
  case http::status::permanent_redirect:
    return true;
  default:
    return false;
  }
}

// What belongs to the first request alone, once the method changes: what describes its body, and its preconditions,
// which the new resource need not meet.
[[nodiscard]] constexpr bool only_for_the_first_method(std::string_view name) noexcept {
  auto const starts_with = [name](std::string_view prefix) {
    return name.size() >= prefix.size() && http::detail::iequals(name.substr(0, prefix.size()), prefix);
  };

  return starts_with("content-") || starts_with("if-") || http::detail::iequals(name, "range") ||
         http::detail::iequals(name, "digest") || http::detail::iequals(name, "etag") ||
         http::detail::iequals(name, "last-modified") || http::detail::iequals(name, "transfer-encoding") ||
         http::detail::iequals(name, "trailer") || http::detail::iequals(name, "expect");
}

// What the request a redirect asks for sends in place of `request`: RFC 9110, 15.4, and what browsers do with a POST.
void redirect_request(http::request &request, http::status status, std::string target) {
  auto const historic = status == http::status::moved_permanently || status == http::status::found;
  auto const becomes_get =
      (status == http::status::see_other && request.method != http::verb::head && request.method != http::verb::get) ||
      (historic && request.method == http::verb::post);

  request.target = std::move(target);

  if (!becomes_get) {
    return;
  }

  http::fields kept;

  for (auto const &entry : request.fields) {
    if (!only_for_the_first_method(entry.name)) {
      kept.add(entry);
    }
  }

  request.method = http::verb::get;
  request.fields = std::move(kept);
  request.body.clear();
  request.trailers.clear();
}

// The `Location` of a response, which takes one value: several that differ say nothing.
[[nodiscard]] std::optional<std::string_view> location_of(http::fields const &fields QUOLL_LIFETIMEBOUND) {
  std::optional<std::string_view> found;

  for (auto const &entry : fields) {
    if (!http::detail::iequals(entry.name, "location")) {
      continue;
    }

    if (found && *found != entry.value) {
      return std::nullopt;
    }

    found = entry.value;
  }

  return found;
}

// Sends `request`, already prepared as `prepared` and given its `Host` field, then the ones its redirects ask for.
[[nodiscard]] async::task<http::response> send_following(std::shared_ptr<client_core> shared, http::request request,
                                                         prepared_request prepared) {
  for (std::size_t followed = 0;; ++followed) {
    auto response = co_await send_prepared(shared, std::move(prepared));
    auto const location = location_of(response.fields);

    if (!redirects(response.status) || !location) {
      co_return response;
    }

    auto target = resolve_location(*location, *request.fields.find("Host"), shared->secure, request.target);

    if (!target) {
      co_return response;
    }

    if (followed == shared->options.max_redirects) {
      throw std::system_error{http::error::too_many_redirects, "quoll::http::client: too many redirects"};
    }

    redirect_request(request, response.status, std::move(*target));
    prepared = prepare(*shared, request);
  }
}

// A response being read in pieces, and its connection: what an http::streamed_response holds. It owns its place in
// max_connections until the body has come to its end, or until it goes. Thread safety: none, on the loop of what reads
// it; what it shares with its client has its own.
class response_stream {
public:
  explicit response_stream(std::shared_ptr<client_core> shared)
      : shared_(std::move(shared)), trailers_(std::make_shared<http::fields>()) {}

  [[nodiscard]] async::detail::held_place &place() noexcept { return place_; }

  // Sends `request` on `connection` and reads the head of its final response. As in exchange(), on an idle connection,
  // false when the request goes again on a new one.
  [[nodiscard]] async::task<bool> start(io::any_stream connection, prepared_request const &request QUOLL_LIFETIMEBOUND,
                                        attempt on) QUOLL_LIFETIMEBOUND {
    sent_at_ = idle_clock(co_await async::current_executor());

    auto const sent = co_await io::write(connection, std::as_bytes(std::span{request.wire}));

    if (sent.error) {
      if (!peer_has_gone(sent.error)) {
        throw std::system_error{sent.error, "quoll::http::client: cannot send the request"};
      }

      // As in exchange(): on an idle connection, an idempotent request goes again, and so does any of which nothing
      // was written.
      if (goes_again_unsent(on, sent.transferred)) {
        co_return false;
      }
    }

    // As in exchange(): a server may answer a request it will not read, and go.
    auto answered = false;

    try {
      answered = co_await read_final_head(connection, request, on);
    } catch (std::system_error const &thrown) {
      if (sent.error && peer_has_gone(thrown.code())) {
        throw std::system_error{sent.error, "quoll::http::client: cannot send the request"};
      }

      throw;
    }

    if (answered) {
      connection_.emplace(std::move(connection));
      // Nor is a connection kept that answered 408 as it was idle: the server may answer the request after it.
      request_keeps_alive_ = request.keep_alive && !sent.error && may_go_on_after(on, parser_->head().status);
    }

    co_return answered;
  }

  // start() for a request with a body in pieces. A head that comes before its end stops it: this is not a conversation,
  // and what is left of the body has nowhere to go while the caller reads.
  [[nodiscard]] async::task<> start_uploading(io::any_stream connection,
                                              prepared_request const &request QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    upload sending{connection, request, shared_->options.response_timeout};

    sent_at_ = idle_clock(co_await async::current_executor());

    try {
      static_cast<void>(co_await async::when_all(
          sending.run(), until_answered(sending, read_final_head(connection, request, attempt::fresh))));
    } catch (std::system_error const &thrown) {
      throw_what_ended(sending, thrown);
    }

    connection_.emplace(std::move(connection));
    request_keeps_alive_ = request.keep_alive && sending.complete();
  }

  // What the caller gets of the head: the rest of the message stays for read(). A message with nothing more to come
  // gives its connection back at once, as nothing will read it to its end.
  [[nodiscard]] http::streamed_response take_head(std::shared_ptr<response_stream> const &self) {
    auto const &message = parser_->head();
    http::streamed_response response{
        message.status, message.reason, message.version, message.fields, http::response_body{self}, trailers_};

    // Nothing more can come: its connection and its place go now, whether the caller reads what came or not, and
    // however the bytes were cut.
    auto const failure = parser_->failure();

    if (parser_->done() || failure) {
      parser_->take_body(piece_);
      held_piece_ = !piece_.empty();
    }

    if (parser_->done()) {
      finish();
    } else if (failure) {
      fail(failure);
    }

    return response;
  }

  [[nodiscard]] async::task<std::optional<std::string_view>> read() QUOLL_LIFETIMEBOUND {
    if (held_piece_) {
      held_piece_ = false;
      co_return std::string_view{piece_};
    }

    if (failure_) {
      throw std::system_error{failure_, "quoll::http::client: cannot read the response"};
    }

    if (ended_) {
      co_return std::nullopt;
    }

    // What a read cut short leaves is no new piece.
    piece_.clear();

    try {
      co_await async::detail::with_optional_timeout(shared_->options.response_timeout, read_piece());
    } catch (std::system_error const &thrown) {
      // A cancellation too: a stream does not take it up again, and over TLS its session is gone.
      fail(thrown.code());
      throw;
    } catch (...) {
      // What it had read may be lost: nothing can follow it.
      fail(std::make_error_code(std::errc::io_error));
      throw;
    }

    // At once, even with a last piece to hand over: its connection and its place are done with, for as long as the
    // caller is at work on that piece. What it leaves alone is the piece.
    if (parser_->done()) {
      finish();
    } else if (auto const failure = parser_->failure()) {
      // Past this piece: the connection can carry nothing more, and the next read says so.
      fail(failure);
    }

    if (piece_.empty()) {
      co_return std::nullopt;
    }

    co_return std::string_view{piece_};
  }

private:
  // Reads the head of the final response, on an attempt of its own; false as start() says.
  [[nodiscard]] async::task<bool> read_final_head(io::any_stream &connection QUOLL_LIFETIMEBOUND,
                                                  prepared_request const &request QUOLL_LIFETIMEBOUND,
                                                  attempt on) QUOLL_LIFETIMEBOUND {
    auto interim = 0;

    // Afresh for each attempt: the one before may have left anything in it.
    buffer_.clear();
    parser_.emplace(shared_->options.limits);
    parser_->set_request_method(request.method);
    // An interim response is read whole: only the final one is the caller's to read.
    parser_->set_body_streaming([](http::response const &head) { return !http::is_informational(head.status); });

    for (;;) {
      if (auto const error = co_await http::read(connection, buffer_, *parser_)) {
        if (on == attempt::idle_again && interim == 0 && peer_has_gone(error)) {
          co_return false;
        }

        throw std::system_error{error, "quoll::http::client: cannot read the response"};
      }

      auto const status = parser_->head().status;

      if (http::is_informational(status) && !parser_->upgrade()) {
        if (++interim > max_interim_responses) {
          throw std::system_error{http::error::bad_message, "quoll::http::client: too many interim responses"};
        }

        // The request method holds until the final one.
        static_cast<void>(parser_->release());
        continue;
      }

      co_return !(interim == 0 && status == http::status::request_timeout &&
                  goes_again_after_408(on, http::detail::closes_connection(parser_->head().fields)));
    }
  }

  // Thrown rather than returned: a read a timeout cancelled is then that timeout's timed_out.
  [[nodiscard]] async::task<> read_piece() QUOLL_LIFETIMEBOUND {
    if (auto const error = co_await http::read_body(*connection_, buffer_, *parser_, piece_)) {
      throw std::system_error{error, "quoll::http::client: cannot read the response"};
    }
  }

  void fail(std::error_code why) noexcept {
    failure_ = why;
    let_go();
  }

  void finish() {
    ended_ = true;

    auto const keep_alive = parser_->keep_alive();
    auto const upgrade = parser_->upgrade();

    *trailers_ = parser_->release().trailers;

    // Bytes past the response would be read as the next one: a connection carrying them is not reused.
    if (request_keeps_alive_ && keep_alive && !upgrade && buffer_.empty()) {
      shared_->pool.give_back(std::move(*connection_), sent_at_);
    }

    let_go();
  }

  // Now rather than with what holds this, for as long as the caller keeps it: the place is for others, the server has
  // no more to send, and what is left to read needs neither the client nor the parser.
  void let_go() noexcept {
    connection_.reset();
    place_.release();
    parser_.reset();
    buffer_ = std::string{};
    shared_.reset();
  }

  std::shared_ptr<client_core> shared_;
  // One per attempt, from start().
  std::optional<http::response_parser> parser_;
  std::shared_ptr<http::fields> trailers_;
  // Before the connection: it closes first, and only then is its place another's.
  async::detail::held_place place_;
  std::optional<io::any_stream> connection_;
  std::string buffer_;
  std::string piece_;
  std::error_code failure_;
  bool request_keeps_alive_ = false;
  bool ended_ = false;
  // What came whole with the head, for the first read.
  bool held_piece_ = false;
  // When its request was sent, on idle_clock().
  std::chrono::nanoseconds sent_at_{};
};

[[nodiscard]] async::task<http::streamed_response> stream_prepared(std::shared_ptr<client_core> shared,
                                                                   prepared_request request) {
  auto stream = std::make_shared<response_stream>(shared);

  if (shared->places) {
    co_await async::detail::with_optional_timeout(shared->options.connect_timeout,
                                                  stream->place().take(*shared->places));
  }

  throw_if_stopped(co_await async::current_stop_token());

  auto const executor = co_await async::current_executor();

  // As in send_prepared().
  if (auto kept = take_idle(*shared, request, executor)) {
    auto const given_back = kept->idle.given_back;
    auto started = false;

    try {
      started = co_await async::detail::with_optional_timeout(
          shared->options.response_timeout, stream->start(std::move(kept->idle.connection), request, kept->on));
    } catch (std::system_error const &thrown) {
      // Closed by the server after some idle time: the connections idle for longer are likely closed as well.
      if (peer_has_gone(thrown.code())) {
        shared->pool.close_older_than(given_back);
      }

      throw;
    }

    if (!started) {
      shared->pool.close_older_than(given_back);
    } else {
      auto response = stream->take_head(stream);

      // A 408 as the response of a request not sent twice: as in send_prepared().
      if (response.status == http::status::request_timeout) {
        shared->pool.close_older_than(given_back);
      }

      co_return response;
    }
  }

  make_room(*shared);

  auto connection = co_await async::detail::with_optional_timeout(shared->options.connect_timeout, shared->open());

  if (request.writer) {
    co_await stream->start_uploading(std::move(connection), request);
    co_return stream->take_head(stream);
  }

  static_cast<void>(co_await async::detail::with_optional_timeout(
      shared->options.response_timeout, stream->start(std::move(connection), request, attempt::fresh)));
  co_return stream->take_head(stream);
}

// What send() and stream() ask of a request before anything runs.
void make_ready(client_core const &core, http::request &request) {
  check_origin_form(request.target);
  request.target.insert(0, core.prefix);

  if (!request.fields.contains("Host")) {
    request.fields.add("Host", core.host_field);
  }
}

[[nodiscard]] async::task<http::streamed_response> stream_connected(std::shared_ptr<client_core> const &core,
                                                                    http::request request) {
  make_ready(*core, request);
  return stream_prepared(core, prepare(*core, request));
}

[[nodiscard]] async::task<http::response> send_connected(std::shared_ptr<client_core> const &core,
                                                         http::request request) {
  make_ready(*core, request);

  auto prepared = prepare(*core, request);

  // A body in pieces cannot be sent again: its 3xx is a result.
  if (core->options.max_redirects == 0 || prepared.writer) {
    return send_prepared(core, std::move(prepared));
  }

  return send_following(core, std::move(request), std::move(prepared));
}

} // namespace

client_backend make_client_backend(connector_handle connector, std::string host, std::uint16_t port, bool secure,
                                   std::string prefix, client_options const &options) {
  auto field = host_field(host, port, secure ? 443 : 80);

  static constexpr client_operations operations{
      [](std::shared_ptr<void> const &state, http::request request) {
        return send_connected(std::static_pointer_cast<client_core>(state), std::move(request));
      },
      [](std::shared_ptr<void> const &state, http::request request) {
        return stream_connected(std::static_pointer_cast<client_core>(state), std::move(request));
      },
  };

  return {std::make_shared<client_core>(std::move(connector), std::move(host), port, secure, std::move(field),
                                        std::move(prefix), options),
          &operations};
}

} // namespace quoll::http::detail

namespace quoll::http {

namespace {

[[nodiscard]] request simple_request(verb method, std::string target, std::string body = {},
                                     std::string_view content_type = {}) {
  request made;

  made.method = method;
  made.target = std::move(target);
  made.body = std::move(body);

  if (!content_type.empty()) {
    made.fields.add("Content-Type", content_type);
  }

  return made;
}

} // namespace

// Here a client from a URL has no sockets: nothing of a connection reaches a page that does not name a connector, which
// the linker can then leave out.
#if defined(__EMSCRIPTEN__)
client::client(std::string_view url, client_options options) : backend_(detail::make_platform_backend(url, options)) {}
#else
client::client(std::string_view url, client_options options)
    : client(io::network_connector{}, url, std::move(options)) {}
#endif

async::task<response> client::get(std::string target) const {
  return send(simple_request(verb::get, std::move(target)));
}

async::task<response> client::head(std::string target) const {
  return send(simple_request(verb::head, std::move(target)));
}

async::task<response> client::delete_(std::string target) const {
  return send(simple_request(verb::delete_, std::move(target)));
}

async::task<response> client::post(std::string target, std::string body, std::string_view content_type) const {
  return send(simple_request(verb::post, std::move(target), std::move(body), content_type));
}

async::task<response> client::put(std::string target, std::string body, std::string_view content_type) const {
  return send(simple_request(verb::put, std::move(target), std::move(body), content_type));
}

async::task<response> client::patch(std::string target, std::string body, std::string_view content_type) const {
  return send(simple_request(verb::patch, std::move(target), std::move(body), content_type));
}

} // namespace quoll::http
