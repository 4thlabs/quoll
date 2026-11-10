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

#include <quoll/server/detail/serve_connection.hpp>

#include <quoll/async/await_context.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/code_of.hpp>
#include <quoll/async/detail/either_stop.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/timeout_children.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/report.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/detail/final_response.hpp>
#include <quoll/http/detail/late_head.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/detail/streamed_head.hpp>
#include <quoll/http/detail/streamed_wire.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace quoll::http {

namespace {

// How reading a request ended.
struct request_read {
  std::error_code error;
  // The request did not arrive in time. Told apart from `error`: a stream reports std::errc::timed_out too, for a peer
  // that has vanished, and that one gets no answer.
  bool expired = false;
};

// The status answering a request that could not be read, if it deserves one: a peer that closed in the middle of a
// message, or a broken stream, gets nothing.
[[nodiscard]] std::optional<http::status> read_error_status(request_read const &read) noexcept {
  if (read.expired) {
    return http::status::request_timeout;
  }

  if (read.error == http::error::header_too_large) {
    return http::status::request_header_fields_too_large;
  }

  if (read.error == http::error::body_too_large) {
    return http::status::content_too_large;
  }

  if (read.error == http::error::unsupported_method) {
    return http::status::not_implemented;
  }

  if (read.error == http::error::unsupported_expectation) {
    return http::status::expectation_failed;
  }

  if (read.error == http::error::bad_message) {
    return http::status::bad_request;
  }

  return std::nullopt;
}

// Drops what comes before a request and is no part of it: the empty lines a server ignores (RFC 9112, 2.2), which some
// clients send after a body.
void skip_empty_lines(std::string &buffer) { buffer.erase(0, buffer.find_first_not_of("\r\n")); }

// What waiting for a request came to: it has started; the wait was ended, by the idle timeout, a drain, a stop request
// or the peer closing its side, which is still owed the end of this one; or the stream has failed.
enum class waited { request, over, failed };

// Waits for a request to start, in `buffer`.
async::task<waited> wait_for_request(io::any_stream &stream, std::string &buffer) {
  std::array<char, 4096> chunk;

  while (buffer.empty()) {
    auto const received = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (received.error) {
      auto const ended = received.error == std::errc::operation_canceled || received.error == io::error::end_of_stream;

      co_return ended ? waited::over : waited::failed;
    }

    buffer.append(chunk.data(), received.transferred);
    skip_empty_lines(buffer);
  }

  co_return waited::request;
}

// What a request's `Expect` fields ask for: a list, of which only this one member exists. An HTTP/1.0 client cannot
// wait for an interim response (RFC 9110, 10.1.1).
enum class expectation { none, a_continue, unsupported };

[[nodiscard]] expectation expectation_of(http::request const &head) noexcept {
  auto found = expectation::none;

  if (head.version == http::version::http_1_0) {
    return found;
  }

  for (auto const &field : head.fields) {
    if (!http::detail::iequals(field.name, "Expect")) {
      continue;
    }

    for (auto const member : std::views::split(std::string_view{field.value}, ',')) {
      auto const name = http::detail::trim(std::string_view{member.begin(), member.end()});

      if (name.empty()) {
        continue;
      }

      if (!http::detail::iequals(name, "100-continue")) {
        return expectation::unsupported;
      }

      found = expectation::a_continue;
    }
  }

  return found;
}

// How long one direction of a message may still wait for its peer: its timeout, less what it waited, plus what the
// bytes that came earn at the minimum rate, never above its timeout, so that a burst does not pay for a long silence
// after it. While an operation goes on, that is one deadline on the timer's clock, which what transfers moves and what
// watches reads: atomic, as an executor running them on several threads may do it at once, though
// a socket asks for one that does not.
class patience {
public:
  // An operation that takes no longer than `prompt` gives back all of the timeout: see paced_stream.
  patience(std::chrono::nanoseconds timeout, std::uint64_t min_rate, std::chrono::nanoseconds prompt) noexcept
      : timeout_(timeout), left_(timeout), prompt_(prompt), min_rate_(min_rate) {}

  [[nodiscard]] bool limited() const noexcept { return timeout_ != std::chrono::nanoseconds::zero(); }

  // The timer of the operation under way, if it is to be timed: what it transfers then counts.
  void timed_by(std::optional<async::timer_ref> timer) noexcept { timer_ = timer; }

  [[nodiscard]] std::optional<async::timer_ref> const &timer() const noexcept { return timer_; }

  // The clock runs against the peer from here to operation_over(): not between two reads of a handler.
  void operation_begins() noexcept {
    began_ = timer_->now();
    deadline_.store(later(began_, left_).count(), std::memory_order_relaxed);
  }

  [[nodiscard]] std::chrono::nanoseconds left_now() const noexcept { return until(deadline(), timer_->now()); }

  void came(std::uint64_t bytes) noexcept {
    if (min_rate_ == 0) {
      return;
    }

    auto const latest = later(timer_->now(), timeout_);
    auto const earned = std::chrono::duration<double>{static_cast<double>(bytes) / static_cast<double>(min_rate_)};

    // Compared as they are: a large write at a low rate may earn more than nanoseconds can hold.
    auto const moved =
        earned >= timeout_
            ? latest
            : std::min(later(deadline(), std::chrono::duration_cast<std::chrono::nanoseconds>(earned)), latest);

    deadline_.store(moved.count(), std::memory_order_relaxed);
  }

  void operation_over() noexcept {
    auto const now = timer_->now();

    left_ = now - began_ <= prompt_ ? timeout_ : until(deadline(), now);
  }

  void start_again() noexcept { left_ = timeout_; }

  // A new piece, for what has no rate to go by: each then has all of the timeout.
  void next_piece() noexcept {
    if (min_rate_ == 0) {
      start_again();
    }
  }

private:
  // `time` plus `more`, where a timeout may be close to all that nanoseconds can hold.
  [[nodiscard]] static std::chrono::nanoseconds later(std::chrono::nanoseconds time,
                                                      std::chrono::nanoseconds more) noexcept {
    return more > std::chrono::nanoseconds::max() - time ? std::chrono::nanoseconds::max() : time + more;
  }

  // What is left to `deadline`, where one that has long passed must stay passed rather than wrap around: a clock is
  // never negative, see async::Timer.
  [[nodiscard]] static std::chrono::nanoseconds until(std::chrono::nanoseconds deadline,
                                                      std::chrono::nanoseconds now) noexcept {
    return deadline < std::chrono::nanoseconds::min() + now ? std::chrono::nanoseconds::min() : deadline - now;
  }

  [[nodiscard]] std::chrono::nanoseconds deadline() const noexcept {
    return std::chrono::nanoseconds{deadline_.load(std::memory_order_relaxed)};
  }

  std::chrono::nanoseconds timeout_;
  std::chrono::nanoseconds left_;
  std::chrono::nanoseconds began_{};
  std::chrono::nanoseconds prompt_;
  std::atomic<std::chrono::nanoseconds::rep> deadline_{0};
  std::uint64_t min_rate_;
  std::optional<async::timer_ref> timer_;
};

// The stream of a connection while a message goes through it. One timer per operation, however many reads or writes it
// takes, as a timer costs allocations: each of them only says what it transferred, and the timer, once it has waited
// for what was left, looks at what is left now. Only an operation counts against the peer, rather than what a handler
// does between two. What it cuts short throws, as timed_out then can only be its own.
//
// Reading, only the bytes of a body earn time, as the parser counts them: a head has its timeout as a whole, and
// framing that no limit bounds, chunk extensions or a chunk size's leading zeros, earns a peer nothing. They count as
// the next read begins, as the parser has not seen them before.
//
// Writing, what takes no longer than `prompt_write` gives back all of the timeout: a peer that takes bytes at once is
// not the one to blame for what a busy loop or an earlier stall cost, which on a long stream of small writes would add
// up to more than they earn. A peer that does not read soon blocks a write for longer. A loop that keeps every write
// waiting for longer than this has other troubles.
//
// Thread safety: one read and one write may go on at once, a body_writer reading its request, as each direction keeps
// to its own patience. `100 Continue` is a write too: it leaves before a writer starts, as whether one is
// owed is for the peer to say, and no handler to know.
class paced_stream {
  static constexpr std::chrono::nanoseconds prompt_write = std::chrono::milliseconds{100};
  // Less than any time an operation takes.
  static constexpr std::chrono::nanoseconds never_prompt{-1};

  // What a transfer is awaited through, with no frame of its own, as it is on the path of every request.
  template <io::io_result (paced_stream::*Transferred)(io::io_result)>
  class [[nodiscard]] transfer {
  public:
    transfer(paced_stream &stream, io::any_stream::awaitable inner) noexcept
        : stream_(&stream), inner_(std::move(inner)) {}

    [[nodiscard]] bool await_ready() { return inner_.await_ready(); }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                                        async::await_context const &context) {
      return inner_.await_suspend(handle, context);
    }

    [[nodiscard]] io::io_result await_resume() { return (stream_->*Transferred)(inner_.await_resume()); }

  private:
    paced_stream *stream_;
    io::any_stream::awaitable inner_;
  };

public:
  paced_stream(io::any_stream &stream QUOLL_LIFETIMEBOUND, http::request_parser const &parser QUOLL_LIFETIMEBOUND,
               connection_options const &options) noexcept
      : stream_(&stream), parser_(&parser), reading_(options.read_timeout, options.min_read_rate, never_prompt),
        writing_(options.write_timeout, options.min_write_rate, prompt_write) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    // Before it waits: what the last read brought has been through the parser by now.
    if (reading_.timer()) {
      reading_.came(body_since_last());
    }

    return transfer<&paced_stream::was_read>{*this, stream_->read_some(buffer)};
  }

  [[nodiscard]] auto write_some(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    return transfer<&paced_stream::was_written>{*this, stream_->write_some(data)};
  }

  // `reading`, which reads through this, under the read patience.
  [[nodiscard]] async::task<std::error_code>
  patiently_reading(async::task<std::error_code> reading) QUOLL_LIFETIMEBOUND {
    return patiently(reading_, std::move(reading));
  }

  // `writing`, which writes through this, under the write patience, with what stopped it as an error.
  [[nodiscard]] async::task<std::error_code>
  patiently_writing(async::task<std::error_code> writing) QUOLL_LIFETIMEBOUND {
    try {
      co_return co_await patiently(writing_, std::move(writing));
    } catch (std::system_error const &thrown) {
      co_return thrown.code();
    }
  }

  // All of `data`, as patiently_writing() says.
  [[nodiscard]] async::task<std::error_code>
  write_all(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    return patiently_writing(write_through(data));
  }

  // A new request: what the parser counts now is its own.
  void message_begins() noexcept { body_seen_ = 0; }

  [[nodiscard]] patience &reading() noexcept QUOLL_LIFETIMEBOUND { return reading_; }

  [[nodiscard]] patience &writing() noexcept QUOLL_LIFETIMEBOUND { return writing_; }

private:
  [[nodiscard]] async::task<std::error_code>
  write_through(std::span<std::byte const> data QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    co_return (co_await io::write(*this, data)).error;
  }

  // What the parser has seen of the body since it was last asked.
  [[nodiscard]] std::uint64_t body_since_last() noexcept {
    auto const received = parser_->body_received();
    auto const since = received - body_seen_;

    body_seen_ = received;
    return since;
  }

  [[nodiscard]] static io::io_result checked(io::io_result done) {
    if (done.error == std::errc::operation_canceled) {
      throw std::system_error{done.error};
    }

    return done;
  }

  [[nodiscard]] io::io_result was_read(io::io_result done) { return checked(done); }

  [[nodiscard]] io::io_result was_written(io::io_result done) {
    static_cast<void>(checked(done));

    if (writing_.timer()) {
      writing_.came(done.transferred);
    }

    return done;
  }

  [[nodiscard]] static async::task<> watch(patience &limit QUOLL_LIFETIMEBOUND,
                                           async::detail::timeout_state &state QUOLL_LIFETIMEBOUND) {
    async::detail::stop_on_exit const stop_work{state.work};
    auto const timer = *limit.timer();

    // Stopped first: what has run out of time then is not what ended it.
    while (!state.watchdog.stop_requested()) {
      auto const left = limit.left_now();

      if (left <= std::chrono::nanoseconds::zero()) {
        state.expired = true;
        co_return;
      }

      if (!co_await async::detail::stop_scope<bool>{timer.wait_for(left), state.watchdog.get_token()}) {
        co_return;
      }
    }
  }

  // with_timeout(), with a timer that looks again at what is left.
  [[nodiscard]] static async::task<std::error_code> patiently(patience &limit QUOLL_LIFETIMEBOUND,
                                                              async::task<std::error_code> work) {
    auto const executor = co_await async::current_executor();
    auto const timer = executor.query<async::timer_ref>();

    limit.timed_by(limit.limited() ? timer : std::nullopt);

    if (!limit.timer()) {
      co_return co_await std::move(work);
    }

    auto const stop_token = co_await async::current_stop_token();
    async::detail::timeout_state state;
    std::stop_callback const forward_stop{stop_token, async::detail::timeout_stop_forwarder{&state}};

    limit.operation_begins();

    auto results = co_await async::when_all(async::detail::timeout_work(std::move(work), state), watch(limit, state));

    limit.operation_over();

    if (auto &done = std::get<0>(results)) {
      co_return *done;
    }

    // A stop of the connection is no timeout, even if the time ran out meanwhile.
    if (state.expired && !stop_token.stop_requested()) {
      throw std::system_error{std::make_error_code(std::errc::timed_out),
                              "quoll::http::server: the peer took too long"};
    }

    co_await async::detail::end_stopped{};
  }

  io::any_stream *stream_;
  http::request_parser const *parser_;
  patience reading_;
  patience writing_;
  std::uint64_t body_seen_ = 0;
};

static_assert(io::ByteStream<paced_stream>);

// A task reading: what its patience or a stop cut short, thrown there, as what a request read says.
async::task<request_read> as_request_read(async::task<std::error_code> reading) {
  try {
    co_return request_read{co_await std::move(reading)};
  } catch (std::system_error const &thrown) {
    if (thrown.code() == std::errc::timed_out) {
      co_return request_read{thrown.code(), true};
    }

    if (thrown.code() == std::errc::operation_canceled) {
      co_return request_read{thrown.code()};
    }

    throw;
  }
}

// RFC 9110, 7.2: uri-host [ ":" port ], or nothing. What a proxy and what is behind it could split otherwise is
// refused: a comma, which is how a proxy may merge two lines, as a second port or a bracket left open.
[[nodiscard]] bool reads_as_a_host(std::string_view text) noexcept {
  auto const name_character = [](char const character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || std::string_view{"-._~%!$&'()*+;="}.contains(character);
  };
  auto const digit = [](char const character) { return character >= '0' && character <= '9'; };

  std::string_view host = text;
  std::string_view port;

  if (text.starts_with('[')) {
    auto const end = text.find(']');

    if (end == std::string_view::npos) {
      return false;
    }

    auto const inside = text.substr(1, end - 1);

    if (!std::ranges::all_of(inside, [](char const character) {
          return std::string_view{"0123456789abcdefABCDEF:.vV-._~!$&'()*+;="}.contains(character);
        })) {
      return false;
    }

    auto const rest = text.substr(end + 1);

    if (!rest.empty() && !rest.starts_with(':')) {
      return false;
    }

    return std::ranges::all_of(rest.empty() ? rest : rest.substr(1), digit);
  }

  if (auto const colon = text.find(':'); colon != std::string_view::npos) {
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
  }

  return std::ranges::all_of(host, name_character) && std::ranges::all_of(port, digit);
}

// RFC 9112, 3.2: an HTTP/1.1 request has one Host, and neither version has two: what is in front and what is behind
// could otherwise read different authorities.
[[nodiscard]] bool host_is_as_it_must(http::request const &head) noexcept {
  auto const hosts =
      std::ranges::count_if(head.fields, [](auto const &entry) { return http::detail::iequals(entry.name, "Host"); });

  if (head.version == http::version::http_1_1 ? hosts != 1 : hosts > 1) {
    return false;
  }

  return reads_as_a_host(http::detail::trim(head.fields.find("Host").value_or("")));
}

// A request, or its head alone when `100 Continue` is owed before the rest: its client waits for that before its body.
// Nothing is owed if the body has come already, or if there is none. A body in pieces stays where it is: its handler
// reads it, and asking for it is what `100 Continue` then says.
async::task<std::error_code> read_up_to_continue(paced_stream &stream, std::string &buffer,
                                                 http::request_parser &parser, bool &continue_owed) {
  if (auto const error = co_await http::read_head(stream, buffer, parser)) {
    co_return error;
  }

  // Past its head, or at its end, as a framing it refuses: no handler sees what is at fault already.
  if (auto const failure = parser.failure()) {
    co_return failure;
  }

  if (!host_is_as_it_must(parser.head())) {
    co_return http::error::bad_message;
  }

  auto const expected = expectation_of(parser.head());

  if (expected == expectation::unsupported) {
    co_return http::error::unsupported_expectation;
  }

  if (parser.done()) {
    co_return std::error_code{};
  }

  if (expected == expectation::a_continue) {
    continue_owed = true;
    co_return std::error_code{};
  }

  if (parser.body_streamed()) {
    co_return std::error_code{};
  }

  co_return co_await http::read(stream, buffer, parser);
}

async::task<std::error_code> write_continue(paced_stream &stream) {
  static constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";

  stream.writing().start_again();
  co_return co_await stream.write_all(std::as_bytes(std::span{interim}));
}

// A whole request, under one read patience, as a slow peer must not get more by sending its head slowly too. Only
// `100 Continue` starts it again, as the time after it is the client's first to send its body. Leaving is under the
// write timeout, like any response. A body in pieces is left to its handler, with `continue_owed` for it.
async::task<request_read> read_whole_request(paced_stream &stream, std::string &buffer, http::request_parser &parser,
                                             bool &continue_owed) {
  stream.message_begins();
  stream.reading().start_again();

  auto const first =
      co_await as_request_read(stream.patiently_reading(read_up_to_continue(stream, buffer, parser, continue_owed)));

  if (first.error || !continue_owed || parser.body_streamed()) {
    co_return first;
  }

  if (auto const sent = co_await write_continue(stream)) {
    co_return request_read{sent};
  }

  stream.reading().start_again();
  co_return co_await as_request_read(stream.patiently_reading(http::read(stream, buffer, parser)));
}

// What reading a body throws through a handler, told apart from what a handler throws on its own by its type, whatever
// its code.
class body_not_read : public std::system_error {
public:
  using std::system_error::system_error;
};

// What a handler taking an http::body_reader reads through, a transfer taking as long as it keeps coming, at the
// minimum rate or with a read timeout per piece. It remembers what stopped it, which says how the request is answered,
// whatever the handler made of it.
class streamed_body {
public:
  streamed_body(paced_stream &stream, std::string &buffer, http::request_parser &parser, bool continue_owed) noexcept
      : stream_(&stream), buffer_(&buffer), parser_(&parser), continue_owed_(continue_owed),
        read_to_its_end_(ends_on_a_next_request(parser)) {}

  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    if (failure_) {
      throw body_not_read{failure_->error, "quoll::http::server: cannot read the body"};
    }

    if (stopped_) {
      throw body_not_read{std::make_error_code(std::errc::operation_canceled),
                          "quoll::http::server: reading the body was stopped"};
    }

    // What a read cut short leaves is no new piece.
    piece_.clear();

    if (no_body_invited_) {
      throw body_not_read{std::make_error_code(std::errc::operation_not_permitted),
                          "quoll::http::server: its answer invited no body"};
    }

    if (continue_owed_) {
      if (auto const failed = co_await continue_before_writing()) {
        failure_ = request_read{failed};
        throw body_not_read{failed, "quoll::http::server: cannot ask for the body"};
      }
    }

    stream_->reading().next_piece();

    auto const attempt = co_await async::stopped_as_optional(
        as_request_read(stream_->patiently_reading(http::read_body(*stream_, *buffer_, *parser_, piece_))));

    read_to_its_end_ = ends_on_a_next_request(*parser_);

    // Ended on a stop, as a handler's own timeout ends it: what follows says the same of a cancellation, and the read
    // ends on the stop.
    if (!attempt) {
      stopped_ = true;
      co_await async::detail::end_stopped{};
    }

    auto const &read = *attempt;

    if (read.error == std::errc::operation_canceled) {
      // Whoever cancels it, as a handler's own timeout does, says what happens next: the body is not at fault, and the
      // response stands. Reading ends, as a stream does not take it up again: over TLS, its session is gone.
      stopped_ = true;
      throw body_not_read{read.error, "quoll::http::server: reading the body was stopped"};
    }

    if (read.error) {
      failure_ = read;
      throw body_not_read{read.error, "quoll::http::server: cannot read the body"};
    }

    // What came with this piece is at fault, whether the handler reads again or not: this request gets no other answer.
    if (auto const failure = parser_->failure()) {
      failure_ = request_read{failure};
    }

    if (piece_.empty()) {
      co_return std::nullopt;
    }

    co_return std::string_view{piece_};
  }

  [[nodiscard]] std::optional<request_read> const &failure() const noexcept { return failure_; }

  // `100 Continue`, if owed, before what may write as this reads: it must not leave beside a piece, and a peer says
  // whether one is owed, which a handler would not know to wait for. So such a route invites its body even if it goes
  // on to answer without it. What fails here leaves a stream nothing more can be written to, stopped or not.
  [[nodiscard]] async::task<std::error_code> continue_before_writing() {
    if (!std::exchange(continue_owed_, false)) {
      co_return std::error_code{};
    }

    auto const failed = co_await write_continue(*stream_);

    if (!failed) {
      stream_->reading().start_again();
    }

    co_return failed;
  }

  // The answer has no use for a body, and invites none: reading then fails at once, rather than wait for what a peer
  // honouring its `Expect` will not send.
  void no_continue() noexcept { no_body_invited_ = std::exchange(continue_owed_, false); }

  // Whether the request has been read whole, and the next one can follow it. For what writes as this reads, which may
  // be on another thread, and so cannot ask the parser.
  [[nodiscard]] bool read_to_its_end() const noexcept { return read_to_its_end_; }

private:
  [[nodiscard]] static bool ends_on_a_next_request(http::request_parser const &parser) noexcept {
    return parser.done() && parser.keep_alive() && !parser.upgrade();
  }

  paced_stream *stream_;
  std::string *buffer_;
  http::request_parser *parser_;
  std::string piece_;
  std::optional<request_read> failure_;
  bool continue_owed_;
  bool no_body_invited_ = false;
  // What writes asks it as this reads, which may be on another thread.
  std::atomic<bool> read_to_its_end_;
  bool stopped_ = false;
};

// What the handler being caught threw, told to the application, but for what a stop throws: no failure, unlike what
// goes wrong during one. Called in a catch block.
// Tells the application's callback, or, with none, writes `what` and the failure rather than lose them: what a handler
// gets wrong is a bug to see.
void tell_handler_error(connection_options const &options, std::string_view what, std::exception_ptr const &failure) {
  if (options.on_handler_error) {
    options.on_handler_error(failure);
  } else {
    async::report(what, failure);
  }
}

// The exception being handled, as tell_handler_error() says, but for a stop the connection was asked for.
void report_failure(connection_options const &options, std::stop_token const &stop_token, std::string_view what) {
  auto const failure = std::current_exception();

  if (stop_token.stop_requested() && async::detail::is_a_stop(failure)) {
    return;
  }

  tell_handler_error(options, what, failure);
}

// The router's response, or 500 if a handler or a middleware threw, reported to the application's callback if it gave
// one.
async::task<http::response> respond(http::router const &router, http::request request,
                                    connection_options const &options, streamed_body *body) {
  auto const stop_token = co_await async::current_stop_token();

  try {
    co_return co_await router.dispatch(std::move(request),
                                       body != nullptr ? http::body_reader{*body} : http::body_reader{});
  } catch (body_not_read const &) {
    // No failure of the handler's, which only let it through.
  } catch (...) {
    report_failure(options, stop_token, "a handler or a middleware threw, answered 500");
  }

  co_return http::detail::status_response(http::status::internal_server_error);
}

// What goes to a peer, or as, HTTP/1.0.
[[nodiscard]] bool speaks_http_1_0(http::response const &response, http::version version) noexcept {
  return version == http::version::http_1_0 || response.version == http::version::http_1_0;
}

// HTTP/1.0 has no chunks.
void drop_chunks(http::response &response) {
  response.fields.erase("Transfer-Encoding");
  response.trailers.clear();
}

// HTTP/1.0 closes unless told otherwise, so that one kept open must say so: its peer would read until the end of it.
// Added to what the field says, as what else it names is hop by hop only as long as it does.
void say_it_stays_open(http::response &response, http::version version, bool keep_alive) {
  if (keep_alive && speaks_http_1_0(response, version) &&
      !http::detail::connection_names(response.fields, "keep-alive")) {
    response.fields.add("Connection", "keep-alive");
  }
}

// Whether a coding other than chunks would go to HTTP/1.0, which has no way to say it: read as the content otherwise.
[[nodiscard]] bool has_no_way_to_say_its_coding(http::response const &response, http::version version) {
  if (!speaks_http_1_0(response, version)) {
    return false;
  }

  return std::ranges::any_of(response.fields, [](auto const &entry) {
    return http::detail::iequals(entry.name, "Transfer-Encoding") &&
           !http::detail::iequals(http::detail::trim(entry.value), "chunked");
  });
}

constexpr auto no_way_to_say_its_coding = "quoll::http::server: a coding other than chunks to HTTP/1.0";

// A handler's answer, unless its peer would read it as something else, the application being told: what is left to
// check once its status and fields are final.
void replace_what_would_mislead(http::response &response, http::verb method, http::version version,
                                connection_options const &options) {
  auto const *wrong = http::detail::replace_what_would_mislead(response, method);

  if (wrong == nullptr && has_no_way_to_say_its_coding(response, version)) {
    wrong = no_way_to_say_its_coding;
    response = http::detail::status_response(http::status::internal_server_error);
  }

  if (wrong != nullptr) {
    tell_handler_error(options, "a handler's response was replaced", std::make_exception_ptr(std::logic_error{wrong}));
  }
}

// How a body of unknown length goes: on HTTP/1.0, nothing but the end of the connection ends it.
void frame_in_pieces(http::response &response, http::version version, bool &keep_alive) {
  if (speaks_http_1_0(response, version)) {
    drop_chunks(response);
    keep_alive = keep_alive && response.fields.contains("Content-Length");
  } else if (!response.fields.contains("Content-Length") && !response.fields.contains("Transfer-Encoding")) {
    response.fields.set("Transfer-Encoding", "chunked");
  }
}

// Ready to write, with a body that comes in pieces. To HEAD, its length is only known if it says so: no framing is
// announced that nothing follows.
void prepare_in_pieces(http::response &response, bool head, bool keep_alive, http::version version) {
  say_it_stays_open(response, version, keep_alive);

  if (head) {
    http::detail::keep_what_head_carries(response, true);
  }

  if (!keep_alive) {
    response.fields.set("Connection", "close");
  }
}

// Ready to write. The last response of a connection says so.
void prepare(http::response &response, bool head, bool keep_alive, http::version version) {
  if (response.body_writer) {
    prepare_in_pieces(response, head, keep_alive, version);
    return;
  }

  say_it_stays_open(response, version, keep_alive);

  if (head) {
    http::detail::keep_what_head_carries(response, false);
  }

  if (!keep_alive) {
    response.fields.set("Connection", "close");
  }
}

// Writes the response, or a 500 in its place if it cannot be serialized (a handler built an invalid one).
async::task<std::error_code> write_response(paced_stream &stream, http::response const &response, bool keep_alive,
                                            http::version version, connection_options const *options) {
  std::optional<async::task<std::error_code>> sending;

  try {
    sending.emplace(http::write(stream, response));
  } catch (std::invalid_argument const &) {
    if (options != nullptr) {
      tell_handler_error(*options, "a handler's response could not be written", std::current_exception());
    }

    auto failed = http::detail::status_response(http::status::internal_server_error);

    prepare(failed, false, keep_alive, version);
    sending.emplace(http::write(stream, failed));
  }

  co_return co_await std::move(*sending);
}

// write_response(), with what stopped it as an error.
async::task<std::error_code> send_response(paced_stream &stream, http::response const &response, bool keep_alive,
                                           http::version version, connection_options const *options = nullptr) {
  stream.writing().start_again();
  co_return co_await stream.patiently_writing(write_response(stream, response, keep_alive, version, options));
}

// What writing a body throws through a writer, told apart from what a writer throws on its own by its type.
class body_not_written : public std::system_error {
public:
  using std::system_error::system_error;
};

// What a body_writer writes through, a transfer taking as long as it keeps leaving, at the minimum rate or with a write
// timeout per piece. The head goes with the first piece, so that a writer failing before its first byte can still be
// answered. A write that fails or is cancelled may have left part of what it had, so that nothing can follow it on this
// connection. Thread safety: one write at a time. What reads the request may run beside it, and shares what is atomic.
class streamed_response {
public:
  streamed_response(paced_stream &stream, std::string head, http::detail::streamed_framing framing) noexcept
      : stream_(&stream), wire_(std::move(head), framing) {}

  [[nodiscard]] async::task<> write(std::string_view piece) {
    if (failed_) {
      throw body_not_written{std::make_error_code(std::errc::broken_pipe),
                             "quoll::http::server: the body cannot go on"};
    }

    if (wire_.past_its_length(piece)) {
      failed_ = true;
      throw std::length_error{"quoll::http::server: a body_writer wrote more than its Content-Length"};
    }

    stream_->writing().next_piece();

    // An empty piece sends what is held back: a stream announces itself before its first data.
    if (auto const bytes = wire_.frame(piece); !bytes.empty()) {
      co_await send(bytes);
    }
  }

  // The end of a body that is whole; false if it is not, or if its end could not leave.
  [[nodiscard]] async::task<bool> finish(http::fields const &trailers) {
    if (failed_ || wire_.short_of_its_length()) {
      co_return false;
    }

    stream_->writing().next_piece();

    auto const bytes = wire_.end(trailers);

    if (bytes.empty()) {
      co_return true;
    }

    failed_ = static_cast<bool>(co_await stream_->write_all(std::as_bytes(std::span{bytes})));
    co_return !failed_;
  }

  // Nothing has left: there is still time to answer something else.
  [[nodiscard]] bool started() const noexcept { return wire_.started(); }

  [[nodiscard]] bool short_of_its_length() const noexcept { return !failed_ && wire_.short_of_its_length(); }

private:
  [[nodiscard]] async::task<> send(std::string_view bytes) {
    if (auto const sent = co_await stream_->write_all(std::as_bytes(std::span{bytes}))) {
      failed_ = true;
      throw body_not_written{sent, "quoll::http::server: cannot write the body"};
    }
  }

  paced_stream *stream_;
  http::detail::streamed_wire wire_;
  bool failed_ = false;
};

// In its place, while nothing has left.
async::task<std::error_code> send_500(paced_stream &stream, bool keep_alive, http::version version,
                                      http::status code = http::status::internal_server_error) {
  auto failed = http::detail::status_response(code);

  prepare(failed, false, keep_alive, version);
  co_return co_await send_response(stream, failed, keep_alive, version);
}

// Runs a response's writer through `sink`, and ends its body if it came out whole. What it returns says what is left to
// do. It holds the writer, whose captures what it runs reads to its end.
enum class writer_end { whole, answer_500, cut };

template <class Sink>
  requires requires(Sink &sink, std::string_view piece, http::fields const &trailers) {
    { sink.write(piece) } -> std::same_as<async::task<>>;
    { sink.finish(trailers) } -> std::same_as<async::task<bool>>;
    { sink.started() } -> std::same_as<bool>;
    { sink.short_of_its_length() } -> std::same_as<bool>;
  }
async::task<writer_end> run_writer(http::body_writer writer, http::response &response, Sink &sink,
                                   connection_options const &options, streamed_body *request_body, bool &nothing_left) {
  auto const stop_token = co_await async::current_stop_token();
  auto complete = false;

  try {
    co_await writer(http::body_stream{sink});
    complete = true;
  } catch (body_not_written const &) {
    // No failure of the writer's, which only let it through.
  } catch (body_not_read const &) {
    // Nor is this, from a writer reading the request.
  } catch (...) {
    report_failure(options, stop_token, "a body_writer threw, its response answered 500 or cut short");
  }

  nothing_left = !sink.started();

  auto const stopped = stop_token.stop_requested();
  auto const request_failed = request_body != nullptr && request_body->failure();

  // A writer that made nothing of a failed request, or that a stop ended, has no whole body to end.
  if (complete && !stopped && !request_failed) {
    if (co_await sink.finish(response.trailers)) {
      co_return writer_end::whole;
    }

    if (sink.short_of_its_length()) {
      tell_handler_error(options, "a response was cut short",
                         std::make_exception_ptr(std::length_error{
                             "quoll::http::server: a body_writer wrote less than its Content-Length"}));
    }
  }

  co_return !sink.started() && !stopped && !request_failed ? writer_end::answer_500 : writer_end::cut;
}

// A response with a body_writer, up to whether the connection can go on: a 500 in its place while nothing has left, and
// otherwise an end of connection for what cannot be whole, which its peer reads as a body cut short. To HEAD, its head
// alone. `nothing_left` says that a failure is still free to answer otherwise.
async::task<bool> send_streamed(paced_stream &stream, http::response &response, bool head, bool keep_alive,
                                http::version version, connection_options const &options, streamed_body *request_body,
                                bool &nothing_left) {
  std::string wire;
  std::optional<http::detail::streamed_framing> framing;

  try {
    framing = http::detail::serialize_streamed_head(response, wire);
  } catch (std::invalid_argument const &) {
    framing.reset();
    tell_handler_error(options, "a handler's response could not be written", std::current_exception());
  }

  auto answer_500 = !framing;

  stream.writing().start_again();

  if (framing && head) {
    co_return !co_await stream.write_all(std::as_bytes(std::span{wire}));
  }

  if (framing) {
    streamed_response sink{stream, std::move(wire), *framing};
    auto const end = co_await run_writer(std::exchange(response.body_writer, nullptr), response, sink, options,
                                         request_body, nothing_left);

    if (end == writer_end::whole) {
      co_return true;
    }

    answer_500 = end == writer_end::answer_500;
  }

  if (!answer_500) {
    co_return false;
  }

  co_return !co_await send_500(stream, keep_alive, version);
}

// What a route with an http::response_writer writes through: the handler gives its status and fields with its first
// write, so that the head is only made then, and what depends on it. On a request whose body it reads, the connection
// goes on only if it has read it all by then. Thread safety: none, as what writes a response takes turns with what
// reads its request.
class late_head_response {
public:
  late_head_response(paced_stream &stream, http::response &response, http::verb method, bool &keep_alive,
                     streamed_body *request_body, http::version version, connection_options const &options) noexcept
      : stream_(&stream), response_(&response), method_(method), head_(method == http::verb::head),
        keep_alive_(&keep_alive), request_body_(request_body), version_(version), options_(&options) {}

  void head(http::status status, http::fields fields) {
    http::detail::merge_head(*response_, status, std::move(fields));
  }

  [[nodiscard]] async::task<> write(std::string_view piece) {
    // Past the first write, a piece costs what it does on any other response.
    return sink_ ? sink_->write(piece) : first_write(piece);
  }

  [[nodiscard]] async::task<bool> finish(http::fields const &trailers) {
    // No write came: nothing has begun that would need an end.
    if (!sink_) {
      co_return true;
    }

    co_return co_await sink_->finish(trailers);
  }

  // Nothing has left: there is still time to answer something else.
  [[nodiscard]] bool started() const noexcept { return head_tried_ || (sink_ && sink_->started()); }

  [[nodiscard]] bool short_of_its_length() const noexcept { return sink_ && sink_->short_of_its_length(); }

  // No write came: its head is all there is.
  [[nodiscard]] bool wrote_nothing() const noexcept { return !head_tried_ && !sink_; }

  [[nodiscard]] bool head_failed() const noexcept { return head_failed_; }

  // What answers in its place, if nothing has left.
  [[nodiscard]] http::status answer_instead() const noexcept { return answer_instead_; }

  // Whether the connection goes on, by what the handler set, read and left to do.
  void decide_keep_alive() { *keep_alive_ = keeps_alive(); }

private:
  // The head leaves with it, and to HEAD, nothing but the head.
  [[nodiscard]] async::task<> first_write(std::string_view piece) {
    if (head_ && head_tried_) {
      throw body_not_written{std::make_error_code(std::errc::operation_canceled),
                             "quoll::http::response_writer: HEAD has no body"};
    }

    // A write made before the one before it ran, as a task starts when awaited, finds what that one opened.
    if (sink_) {
      co_await sink_->write(piece);
      co_return;
    }

    open();

    if (head_) {
      head_tried_ = true;

      if (auto const sent = co_await stream_->write_all(std::as_bytes(std::span{wire_}))) {
        head_failed_ = true;
        throw body_not_written{sent, "quoll::http::response_writer: cannot write the head"};
      }

      throw body_not_written{std::make_error_code(std::errc::operation_canceled),
                             "quoll::http::response_writer: HEAD has no body"};
    }

    co_await sink_->write(piece);
  }

  [[nodiscard]] bool keeps_alive() const {
    auto const going_on = request_body_ != nullptr ? request_body_->read_to_its_end() : *keep_alive_;

    return going_on && !http::detail::closes_connection(response_->fields) && !options_->drain.stop_requested();
  }

  // Makes the head on a copy, so that a handler catching what this throws finds its response as it was: it may then end
  // without writing. Throws what a head that cannot leave throws, which answers 500, as nothing has left.
  void open() {
    // What it gives with its first write has not been through replace_what_would_mislead().
    if (auto const *const wrong = http::detail::status_would_mislead(response_->status, method_); wrong != nullptr) {
      // The same answer as to one known before it ran.
      if (!http::is_informational(response_->status)) {
        answer_instead_ = http::status::not_implemented;
      }

      throw std::logic_error{wrong};
    }

    if (has_no_way_to_say_its_coding(*response_, version_)) {
      throw std::logic_error{no_way_to_say_its_coding};
    }

    auto keep_alive = keeps_alive();
    http::response made;

    made.status = response_->status;
    made.reason = response_->reason;
    made.version = response_->version;
    made.fields = response_->fields;
    made.trailers = response_->trailers;

    if (!head_) {
      frame_in_pieces(made, version_, keep_alive);
    }

    prepare_in_pieces(made, head_, keep_alive, version_);

    std::string wire;
    auto const framing = http::detail::serialize_streamed_head(made, wire);

    *keep_alive_ = keep_alive;
    response_->fields = std::move(made.fields);
    response_->trailers = std::move(made.trailers);

    if (head_) {
      wire_ = std::move(wire);
    } else {
      sink_.emplace(*stream_, std::move(wire), framing);
    }
  }

  paced_stream *stream_;
  http::response *response_;
  http::verb method_;
  bool head_;
  bool *keep_alive_;
  streamed_body *request_body_;
  http::version version_;
  connection_options const *options_;
  std::string wire_;
  std::optional<streamed_response> sink_;
  bool head_tried_ = false;
  bool head_failed_ = false;
  http::status answer_instead_ = http::status::internal_server_error;
};

// A response whose handler writes it, up to whether the connection can go on, as send_streamed().
async::task<bool> send_written_by_handler(paced_stream &stream, http::response &response, http::verb method,
                                          bool &keep_alive, http::version version, connection_options const &options,
                                          streamed_body *request_body, bool &nothing_left) {
  stream.writing().start_again();

  auto const head = method == http::verb::head;
  late_head_response sink{stream, response, method, keep_alive, request_body, version, options};
  auto const end = co_await run_writer(std::exchange(response.late_head_writer, nullptr), response, sink, options,
                                       request_body, nothing_left);

  // To HEAD, the head is all it owes, however the handler took what its write threw.
  if (head && !sink.wrote_nothing()) {
    co_return !sink.head_failed();
  }

  // What a handler asked of the connection stands, whatever replaces its answer.
  if (!sink.started()) {
    sink.decide_keep_alive();
  }

  if (end == writer_end::answer_500) {
    co_return !co_await send_500(stream, keep_alive, version, sink.answer_instead());
  }

  if (end == writer_end::cut) {
    co_return false;
  }

  // It ended before any write: its response has what it set, and an empty body.
  if (sink.wrote_nothing()) {
    replace_what_would_mislead(response, method, version, options);

    if (speaks_http_1_0(response, version)) {
      drop_chunks(response);
    }

    prepare(response, head, keep_alive, version);
    co_return !co_await send_response(stream, response, keep_alive, version, &options);
  }

  co_return true;
}

// How a connection ends. A stream that failed or a peer that has gone are owed nothing: on a stuck one, trying would
// take another write timeout. A connection whose wait was ended stops sending, and nothing is on its way yet. After a
// last response, what is on its way is read, in stages as RFC 9112, 9.6 asks.
enum class connection_end { nothing_owed, stop_sending, linger };

// What takes a connection over once it has switched protocols, and what came on it after the request: no handler, no
// switch.
struct taken_over {
  http::upgrade_handler handler;
  std::string received;
};

// What is wrong with taking the connection over with `response`, or nullptr. Only a 101 answering a request that asked
// to switch, in HTTP/1.1 and with nothing of its own to follow it, can: the peer would read the protocol's bytes as
// something else otherwise.
[[nodiscard]] char const *cannot_switch(http::response const &response, bool asked, http::verb method,
                                        http::version version) noexcept {
  if (!asked || method == http::verb::head || speaks_http_1_0(response, version)) {
    return "quoll::http::server: a protocol switch the request did not ask for";
  }

  if (response.status != http::status::switching_protocols) {
    return "quoll::http::server: a protocol switch with a status other than 101";
  }

  if (!response.body.empty() || response.body_writer || response.late_head_writer) {
    return "quoll::http::server: a protocol switch with a body";
  }

  return nullptr;
}

async::task<std::error_code> stop_sending(io::any_stream &stream) { co_return (co_await stream.shutdown_send()).error; }

async::task<io::io_result> read_once(io::any_stream &stream, std::span<std::byte> buffer) {
  co_return co_await stream.read_some(buffer);
}

// What the peer still sends, dropped, until it stops or goes quiet. A larger buffer than a request's: this is all it
// does.
async::task<std::error_code> drop_what_comes(io::any_stream &stream, std::chrono::nanoseconds silence) {
  std::array<char, 16 * 1024> chunk;

  for (;;) {
    auto const received =
        co_await async::with_timeout(silence, read_once(stream, std::as_writable_bytes(std::span{chunk})));

    if (received.error) {
      co_return received.error;
    }
  }
}

// The 101 written, or nothing if it may not be (see cannot_switch()) or cannot be serialized, the application being
// told: it must not switch then, as a 500 goes in its place.
[[nodiscard]] std::optional<async::task<std::error_code>> switch_protocols(paced_stream &stream,
                                                                           http::response const &response, bool asked,
                                                                           http::verb method, http::version version,
                                                                           connection_options const &options) {
  if (auto const *const wrong = cannot_switch(response, asked, method, version)) {
    tell_handler_error(options, "a handler's switch of protocols was refused, answered 500",
                       std::make_exception_ptr(std::logic_error{wrong}));

    return std::nullopt;
  }

  try {
    return http::write(stream, response);
  } catch (std::invalid_argument const &) {
    tell_handler_error(options, "a handler's switch of protocols could not be written", std::current_exception());

    return std::nullopt;
  }
}

// Requests and their responses, up to how the connection ends.
async::task<connection_end> serve_requests(io::any_stream &stream, http::router const &router,
                                           connection_options const &options, taken_over &taken) {
  std::string buffer;
  http::request_parser parser{options.limits};
  paced_stream paced{stream, parser, options};

  // What ends the wait for a request: the task's stop request, or a request to drain.
  async::detail::either_stop const idle_stop{co_await async::current_stop_token(), options.drain};

  parser.set_body_streaming([&router](http::request const &head) { return router.reads_body_in_pieces(head); });

  // A request that cannot be read: answered if it deserves it, and the connection ends.
  auto const refuse = [&](request_read const &read) -> async::task<connection_end> {
    auto const answer = read_error_status(read);

    if (!answer) {
      co_return connection_end::nothing_owed;
    }

    auto rejected = http::detail::status_response(*answer);

    prepare(rejected, false, false, http::version::http_1_1);
    co_return co_await send_response(paced, rejected, false, http::version::http_1_1) ? connection_end::nothing_owed
                                                                                      : connection_end::linger;
  };

  for (;;) {
    skip_empty_lines(buffer);

    // A pipelined request has already started.
    if (buffer.empty()) {
      // A drain, a stop or the idle timeout ends the wait, as the peer closing its side does: what follows is owed the
      // end of this one.
      auto wait = waited::over;

      try {
        wait = (co_await async::detail::owned_chain<waited>{
                    async::detail::with_optional_timeout(options.idle_timeout, wait_for_request(stream, buffer)),
                    idle_stop.token()})
                   .value_or(waited::over);
      } catch (std::system_error const &thrown) {
        if (thrown.code() != std::errc::timed_out) {
          throw;
        }
      }

      if (wait != waited::request) {
        co_return wait == waited::over ? connection_end::stop_sending : connection_end::nothing_owed;
      }
    }

    bool continue_owed = false;

    if (auto const read = co_await read_whole_request(paced, buffer, parser, continue_owed); read.error) {
      co_return co_await refuse(read);
    }

    std::optional<streamed_body> body;
    auto keep_alive = false;
    // Whether the request asks to switch protocols, its body read: a body still coming is not.
    auto asks_to_switch = false;
    http::request request;

    if (parser.body_streamed()) {
      // The parser goes on with the body: the handler gets a copy of the head.
      request = parser.head();
      body.emplace(paced, buffer, parser, continue_owed);
    } else {
      auto const upgrade = parser.upgrade();

      keep_alive = parser.keep_alive() && !upgrade;
      request = parser.release();
      // llhttp says so of every CONNECT too, which names no protocol: a switch is only to one an Upgrade field names
      // (RFC 9110, 7.8), and a CONNECT is a tunnel, which is not offered.
      asks_to_switch = upgrade && request.method != http::verb::connect && request.fields.contains("Upgrade");
    }

    auto const method = request.method;
    auto const head = method == http::verb::head;
    auto const version = request.version;
    auto response = co_await respond(router, std::move(request), options, body ? &*body : nullptr);

    if (body) {
      // Stopped while it read: nothing is owed, not even the 500 of a handler that let it through.
      if ((co_await async::current_stop_token()).stop_requested()) {
        co_return connection_end::nothing_owed;
      }

      if (body->failure()) {
        co_return co_await refuse(*body->failure());
      }

      // What it left unread, or could not tell apart from the next request, ends the connection.
      keep_alive = body->read_to_its_end();
    }

    if (response.upgrade) {
      if (options.drain.stop_requested()) {
        // No connection to start as the server stops: the client may try another.
        response = http::detail::status_response(http::status::service_unavailable);
      } else if (auto switching = switch_protocols(paced, response, asks_to_switch, method, version, options)) {
        paced.writing().start_again();

        if (co_await paced.patiently_writing(std::move(*switching))) {
          co_return connection_end::nothing_owed;
        }

        // What came after the request is the new protocol's.
        taken.handler = std::move(response.upgrade);
        taken.received = std::move(buffer);
        co_return connection_end::nothing_owed;
      } else {
        response = http::detail::status_response(http::status::internal_server_error);
      }
    }

    keep_alive = keep_alive && !http::detail::closes_connection(response.fields) && !options.drain.stop_requested();

    // After that line: what a handler asked of the connection stands, whatever replaces its answer.
    replace_what_would_mislead(response, method, version, options);

    auto const in_pieces = static_cast<bool>(response.body_writer);
    auto const written_by_handler = !in_pieces && response.late_head_writer;

    if (in_pieces && !head) {
      frame_in_pieces(response, version, keep_alive);
    } else if (!in_pieces && !written_by_handler && speaks_http_1_0(response, version)) {
      drop_chunks(response);
    }

    if (!written_by_handler) {
      prepare(response, head, keep_alive, version);
    }

    if (in_pieces || written_by_handler) {
      // What runs may read as it writes. A status of refusal already set invites no body, nor does a writer that does
      // not run to HEAD.
      if (body && ((in_pieces && head) || static_cast<std::uint16_t>(response.status) >= 300)) {
        body->no_continue();
      } else if (body) {
        if (auto const failed = co_await body->continue_before_writing()) {
          co_return connection_end::nothing_owed;
        }
      }

      auto nothing_left = false;
      auto *const request_body = body ? &*body : nullptr;
      auto sent = false;

      if (written_by_handler) {
        sent = co_await send_written_by_handler(paced, response, method, keep_alive, version, options, request_body,
                                                nothing_left);
      } else {
        sent = co_await send_streamed(paced, response, head, keep_alive, version, options, request_body, nothing_left);
      }

      if (!sent) {
        // A writer reading the request found it at fault, before the response started.
        if (body && body->failure() && nothing_left) {
          co_return co_await refuse(*body->failure());
        }

        co_return connection_end::nothing_owed;
      }
    } else if (co_await send_response(paced, response, keep_alive, version, &options)) {
      co_return connection_end::nothing_owed;
    }

    // A writer may have read the request: it is only over now.
    if (body && keep_alive) {
      static_cast<void>(parser.release());
    }

    if (!keep_alive) {
      co_return connection_end::linger;
    }
  }
}

// Reads and drops what the peer still sends, unless it is time for the connection to go.
async::task<> linger(io::any_stream &stream, connection_options const &options) {
  auto const stop_token = co_await async::current_stop_token();
  auto const executor = co_await async::current_executor();
  auto const zero = std::chrono::nanoseconds::zero();

  // Without a timer to end it, this would wait for as long as the peer likes.
  if (options.lingering_timeout <= zero || options.lingering_time <= zero || !executor.query<async::timer_ref>() ||
      options.drain.stop_requested()) {
    co_return;
  }

  async::detail::either_stop const stop{stop_token, options.drain};

  // Its stop, or a drain, ends it here, as the time running out does.
  (void)co_await async::detail::owned_chain<std::error_code>{
      async::with_timeout(options.lingering_time, drop_what_comes(stream, options.lingering_timeout)), stop.token()};
}

// The last of a connection, once all that it owed has left.
async::task<> end_connection(io::any_stream &stream, connection_options const &options, connection_end end) {
  if (end == connection_end::nothing_owed || (co_await async::current_stop_token()).stop_requested()) {
    co_return;
  }

  try {
    auto const sent = co_await async::detail::with_optional_timeout(options.write_timeout, stop_sending(stream));

    if (!sent && end == connection_end::linger) {
      co_await linger(stream, options);
    }
  } catch (std::system_error const &) {
    // The time ran out, the stop came or the stream failed: with the connection ending anyway, nothing is left to do.
  }
}

// What the peer of a connection taken over causes at will, and lets out of a handler reading it: its stream gone or
// cut, a message that breaks the protocol, a keep-alive given up on it. No bug of the handler's, which only let it
// through.
[[nodiscard]] bool caused_by_its_peer(std::exception_ptr const &failure) {
  auto const code = async::detail::code_of(failure);

  if (!code) {
    return false;
  }

  return code->category() == http::http_category() || code->category() == io::io_category() ||
         *code == std::errc::connection_reset || *code == std::errc::broken_pipe ||
         *code == std::errc::connection_aborted || *code == std::errc::timed_out;
}

// The connection, run by what took it over, told of a drain, with what it throws reported as a handler's would be.
async::task<> run_taken_over(io::any_stream stream, taken_over taken, connection_options const &options) {
  auto const stop_token = co_await async::current_stop_token();

  try {
    co_await taken.handler(std::move(stream), std::move(taken.received), options.drain);
  } catch (...) {
    // The application's callback still hears of it; written out with none, as a peer may cause it at will, it is not.
    if (options.on_handler_error || !caused_by_its_peer(std::current_exception())) {
      report_failure(options, stop_token, "the handler of a connection taken over, as a WebSocket, threw");
    }
  }
}

} // namespace

namespace detail {

async::task<> serve_connection(io::any_stream stream, http::router const &router, connection_options const &options) {
  taken_over taken;
  auto const end = co_await serve_requests(stream, router, options, taken);

  if (taken.handler) {
    co_await run_taken_over(std::move(stream), std::move(taken), options);
    co_return;
  }

  co_await end_connection(stream, options, end);
}

} // namespace detail

} // namespace quoll::http
