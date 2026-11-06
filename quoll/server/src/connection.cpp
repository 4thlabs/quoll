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

#include <quoll/server/serve_connection.hpp>

#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
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
#include <quoll/io/error.hpp>
#include <quoll/io/write.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::server {

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

// A task reading, its cancellation thrown: with_timeout() only tells its own from another when it is.
async::task<std::error_code> throw_canceled(async::task<std::error_code> reading) {
  auto const error = co_await std::move(reading);

  if (error == std::errc::operation_canceled) {
    throw std::system_error{error};
  }

  co_return error;
}

// A task reading, up to the read timeout. Streams report their errors as values: thrown, timed_out can only be the
// timeout's.
async::task<request_read> within_read_timeout(async::task<std::error_code> reading, connection_options const &options) {
  try {
    co_return request_read{
        co_await async::detail::with_optional_timeout(options.read_timeout, throw_canceled(std::move(reading)))};
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

// A request, or its head alone when `100 Continue` is owed before the rest: its client waits for that before its body.
// Nothing is owed if the body has come already, or if there is none. A body in pieces stays where it is: its handler
// reads it, and asking for it is what `100 Continue` then says.
async::task<std::error_code> read_up_to_continue(io::any_stream &stream, std::string &buffer,
                                                 http::request_parser &parser, bool &continue_owed) {
  if (auto const error = co_await http::read_head(stream, buffer, parser)) {
    co_return error;
  }

  // Past its head, or at its end, as a framing it refuses: no handler sees what is at fault already.
  if (auto const failure = parser.failure()) {
    co_return failure;
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

async::task<std::error_code> write_continue(io::any_stream &stream) {
  static constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";

  co_return (co_await io::write(stream, std::as_bytes(std::span{interim}))).error;
}

// A whole request, under one read timeout, as a slow peer must not get more by sending its head slowly too. Only
// `100 Continue` starts it again, as the time after it is the client's first to send its body. Leaving is under the
// write timeout, like any response. A body in pieces is left to its handler, with `continue_owed` for it.
async::task<request_read> read_whole_request(io::any_stream &stream, std::string &buffer, http::request_parser &parser,
                                             connection_options const &options, bool &continue_owed) {
  auto const first = co_await within_read_timeout(read_up_to_continue(stream, buffer, parser, continue_owed), options);

  if (first.error || !continue_owed || parser.body_streamed()) {
    co_return first;
  }

  if (auto const sent = co_await async::detail::with_optional_timeout(options.write_timeout, write_continue(stream))) {
    co_return request_read{sent};
  }

  co_return co_await within_read_timeout(http::read(stream, buffer, parser), options);
}

// What reading a body throws through a handler, told apart from what a handler throws on its own by its type, whatever
// its code.
class body_not_read : public std::system_error {
public:
  using std::system_error::system_error;
};

// What a handler taking an http::body_reader reads through, one read timeout per piece: a transfer takes as long as it
// keeps coming. It remembers what stopped it, which says how the request is answered, whatever the handler made of it.
class streamed_body {
public:
  streamed_body(io::any_stream &stream, std::string &buffer, http::request_parser &parser,
                connection_options const &options, bool continue_owed) noexcept
      : stream_(&stream), buffer_(&buffer), parser_(&parser), options_(&options), continue_owed_(continue_owed) {}

  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    if (failure_) {
      throw body_not_read{failure_->error, "quoll::server: cannot read the body"};
    }

    if (stopped_) {
      throw body_not_read{std::make_error_code(std::errc::operation_canceled),
                          "quoll::server: reading the body was stopped"};
    }

    // What a read cut short leaves is no new piece.
    piece_.clear();

    if (continue_owed_) {
      continue_owed_ = false;
      co_await send_continue();
    }

    auto const read = co_await within_read_timeout(http::read_body(*stream_, *buffer_, *parser_, piece_), *options_);

    if (read.error == std::errc::operation_canceled) {
      // Whoever cancels it, as a handler's own timeout does, says what happens next: the body is not at fault, and the
      // response stands. Reading ends, as a stream does not take it up again: over TLS, its session is gone.
      stopped_ = true;
      throw body_not_read{read.error, "quoll::server: reading the body was stopped"};
    }

    if (read.error) {
      failure_ = read;
      throw body_not_read{read.error, "quoll::server: cannot read the body"};
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

  // The final response is leaving: an interim one can no longer come before it.
  void response_started() noexcept { continue_owed_ = false; }

private:
  // Stopped or not, a `100 Continue` that did not leave whole leaves a stream nothing more can be written to.
  async::task<> send_continue() {
    std::error_code sent;

    try {
      sent = co_await async::detail::with_optional_timeout(options_->write_timeout, write_continue(*stream_));
    } catch (std::system_error const &thrown) {
      sent = thrown.code();
    }

    if (sent) {
      failure_ = request_read{sent};
      throw body_not_read{sent, "quoll::server: cannot ask for the body"};
    }
  }

  io::any_stream *stream_;
  std::string *buffer_;
  http::request_parser *parser_;
  connection_options const *options_;
  std::string piece_;
  std::optional<request_read> failure_;
  bool continue_owed_;
  bool stopped_ = false;
};

// The router's response, or 500 if a handler or a middleware threw, reported to the application's callback if it gave
// one.
async::task<http::response> respond(http::router const &router, http::request request,
                                    connection_options const &options, streamed_body *body) {
  try {
    co_return co_await router.dispatch(std::move(request),
                                       body != nullptr ? http::body_reader{*body} : http::body_reader{});
  } catch (body_not_read const &) {
    // No failure of the handler's, which only let it through.
  } catch (...) {
    if (options.on_handler_error) {
      options.on_handler_error(std::current_exception());
    }
  }

  co_return http::detail::status_response(http::status::internal_server_error);
}

// Ready to write. A response to HEAD has no body: it announces the length of the one it does not carry, with
// Content-Length rather than chunked coding, whose terminating chunk would be read as the next response. The last
// response of a connection says so.
void prepare(http::response &response, bool head, bool keep_alive) {
  if (head && response.body_writer) {
    // Its length is only known if it says so: no framing is announced that nothing follows.
    response.fields.erase("Transfer-Encoding");
    response.body.clear();
    response.trailers.clear();
  } else if (head) {
    if (response.fields.contains("Transfer-Encoding")) {
      response.fields.erase("Transfer-Encoding");
      response.fields.set("Content-Length", std::to_string(response.body.size()));
    } else if (!response.body.empty() && !response.fields.contains("Content-Length")) {
      response.fields.set("Content-Length", std::to_string(response.body.size()));
    }

    response.body.clear();
    response.trailers.clear();
  }

  if (!keep_alive) {
    response.fields.set("Connection", "close");
  }
}

// Writes the response, or a 500 in its place if it cannot be serialized (a handler built an invalid one).
async::task<std::error_code> write_response(io::any_stream &stream, http::response const &response, bool keep_alive) {
  std::optional<async::task<std::error_code>> sending;

  try {
    sending.emplace(http::write(stream, response));
  } catch (std::invalid_argument const &) {
    auto failed = http::detail::status_response(http::status::internal_server_error);

    prepare(failed, false, keep_alive);
    sending.emplace(http::write(stream, failed));
  }

  co_return co_await std::move(*sending);
}

// write_response(), up to the timeout: past it, the error is the stream's cancellation.
async::task<std::error_code> send_response(io::any_stream &stream, http::response const &response, bool keep_alive,
                                           connection_options const &options) {
  co_return co_await async::detail::with_optional_timeout(options.write_timeout,
                                                          write_response(stream, response, keep_alive));
}

// What writing a body throws through a writer, told apart from what a writer throws on its own by its type.
class body_not_written : public std::system_error {
public:
  using std::system_error::system_error;
};

async::task<std::error_code> write_bytes(io::any_stream &stream, std::span<std::byte const> bytes) {
  co_return (co_await io::write(stream, bytes)).error;
}

// What a body_writer writes through, one write timeout per piece: a transfer takes as long as it keeps leaving. The
// head goes with the first piece, so that a writer failing before its first byte can still be answered. A write that
// fails or is cancelled may have left part of what it had, so that nothing can follow it on this connection.
class streamed_response {
public:
  streamed_response(io::any_stream &stream, connection_options const &options, std::string head,
                    http::detail::streamed_framing framing, streamed_body *request_body) noexcept
      : stream_(&stream), options_(&options), request_body_(request_body), wire_(std::move(head), framing) {}

  [[nodiscard]] async::task<> write(std::string_view piece) {
    if (failed_) {
      throw body_not_written{std::make_error_code(std::errc::broken_pipe), "quoll::server: the body cannot go on"};
    }

    if (wire_.past_its_length(piece)) {
      failed_ = true;
      throw std::length_error{"quoll::server: a body_writer wrote more than its Content-Length"};
    }

    response_starts();

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

    response_starts();

    auto const bytes = wire_.end(trailers);

    if (bytes.empty()) {
      co_return true;
    }

    failed_ = static_cast<bool>(co_await send_within_timeout(bytes));
    co_return !failed_;
  }

  // Nothing has left: there is still time to answer something else.
  [[nodiscard]] bool started() const noexcept { return wire_.started(); }

  [[nodiscard]] bool short_of_its_length() const noexcept { return !failed_ && wire_.short_of_its_length(); }

private:
  void response_starts() {
    if (!wire_.started() && request_body_ != nullptr) {
      request_body_->response_started();
    }
  }

  [[nodiscard]] async::task<std::error_code> send_within_timeout(std::string_view bytes) {
    std::error_code sent;

    try {
      sent = co_await async::detail::with_optional_timeout(options_->write_timeout,
                                                           write_bytes(*stream_, std::as_bytes(std::span{bytes})));
    } catch (std::system_error const &thrown) {
      sent = thrown.code();
    }

    co_return sent;
  }

  [[nodiscard]] async::task<> send(std::string_view bytes) {
    if (auto const sent = co_await send_within_timeout(bytes)) {
      failed_ = true;
      throw body_not_written{sent, "quoll::server: cannot write the body"};
    }
  }

  io::any_stream *stream_;
  connection_options const *options_;
  streamed_body *request_body_;
  http::detail::streamed_wire wire_;
  bool failed_ = false;
};

// A response with a body_writer, up to whether the connection can go on: a 500 in its place while nothing has left, and
// otherwise an end of connection for what cannot be whole, which its peer reads as a body cut short. To HEAD, its head
// alone. `nothing_left` says that a failure is still free to answer otherwise.
async::task<bool> send_streamed(io::any_stream &stream, http::response &response, bool head, bool keep_alive,
                                connection_options const &options, streamed_body *request_body, bool &nothing_left) {
  std::string wire;
  std::optional<http::detail::streamed_framing> framing;

  try {
    framing = http::detail::serialize_streamed_head(response, wire);
  } catch (std::invalid_argument const &) {
    framing.reset();
  }

  auto answer_500 = !framing;

  if (framing && head) {
    co_return !co_await async::detail::with_optional_timeout(options.write_timeout,
                                                             write_bytes(stream, std::as_bytes(std::span{wire})));
  }

  if (framing) {
    auto const stop_token = co_await async::current_stop_token();
    streamed_response sink{stream, options, std::move(wire), *framing, request_body};
    auto complete = false;

    try {
      co_await response.body_writer(http::body_stream{sink});
      complete = true;
    } catch (body_not_written const &) {
      // No failure of the writer's, which only let it through.
    } catch (body_not_read const &) {
      // Nor is this, from a writer reading the request.
    } catch (...) {
      // A stop is no failure either.
      if (options.on_handler_error && !stop_token.stop_requested()) {
        options.on_handler_error(std::current_exception());
      }
    }

    nothing_left = !sink.started();

    auto const stopped = stop_token.stop_requested();
    auto const request_failed = request_body != nullptr && request_body->failure();

    // A writer that made nothing of a failed request, or that a stop ended, has no whole body to end.
    if (complete && !stopped && !request_failed) {
      if (co_await sink.finish(response.trailers)) {
        co_return true;
      }

      if (sink.short_of_its_length() && options.on_handler_error) {
        options.on_handler_error(std::make_exception_ptr(
            std::length_error{"quoll::server: a body_writer wrote less than its Content-Length"}));
      }
    }

    answer_500 = !sink.started() && !stopped && !request_failed;
  }

  if (!answer_500) {
    co_return false;
  }

  auto failed = http::detail::status_response(http::status::internal_server_error);

  prepare(failed, false, keep_alive);
  co_return !co_await send_response(stream, failed, keep_alive, options);
}

// How a connection ends. A stream that failed or a peer that has gone are owed nothing: on a stuck one, trying would
// take another write timeout. A connection whose wait was ended stops sending, and nothing is on its way yet. After a
// last response, what is on its way is read, in stages as RFC 9112, 9.6 asks.
enum class connection_end { nothing_owed, stop_sending, linger };

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

// Requests and their responses, up to how the connection ends.
async::task<connection_end> serve_requests(io::any_stream &stream, http::router const &router,
                                           connection_options const &options) {
  std::string buffer;
  http::request_parser parser{options.limits};

  // What ends the wait for a request: the task's stop request, or a request to drain.
  std::stop_source idle_stop;
  auto const stop_waiting = [&idle_stop] { idle_stop.request_stop(); };
  std::stop_callback const on_stop{co_await async::current_stop_token(), stop_waiting};
  std::stop_callback const on_drain{options.drain, stop_waiting};

  parser.set_body_streaming([&router](http::request const &head) { return router.reads_body_in_pieces(head); });

  // A request that cannot be read: answered if it deserves it, and the connection ends.
  auto const refuse = [&](request_read const &read) -> async::task<connection_end> {
    auto const answer = read_error_status(read);

    if (!answer) {
      co_return connection_end::nothing_owed;
    }

    auto rejected = http::detail::status_response(*answer);

    prepare(rejected, false, false);
    co_return co_await send_response(stream, rejected, false, options) ? connection_end::nothing_owed
                                                                       : connection_end::linger;
  };

  for (;;) {
    skip_empty_lines(buffer);

    // A pipelined request has already started.
    if (buffer.empty()) {
      auto const wait = co_await async::detail::stop_scope<waited>{
          async::detail::with_optional_timeout(options.idle_timeout, wait_for_request(stream, buffer)),
          idle_stop.get_token()};

      if (wait != waited::request) {
        co_return wait == waited::over ? connection_end::stop_sending : connection_end::nothing_owed;
      }
    }

    bool continue_owed = false;

    if (auto const read = co_await read_whole_request(stream, buffer, parser, options, continue_owed); read.error) {
      co_return co_await refuse(read);
    }

    std::optional<streamed_body> body;
    auto keep_alive = false;
    http::request request;

    if (parser.body_streamed()) {
      // The parser goes on with the body: the handler gets a copy of the head.
      request = parser.head();
      body.emplace(stream, buffer, parser, options, continue_owed);
    } else {
      keep_alive = parser.keep_alive() && !parser.upgrade();
      request = parser.release();
    }

    auto const head = request.method == http::verb::head;
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
      keep_alive = parser.done() && parser.keep_alive() && !parser.upgrade();
    }

    keep_alive = keep_alive && !http::detail::closes_connection(response.fields) && !options.drain.stop_requested();

    auto const in_pieces = static_cast<bool>(response.body_writer);

    if (in_pieces && !head) {
      // HTTP/1.0 has no chunks: nothing but the end of the connection ends a body of unknown length.
      if (version == http::version::http_1_0 || response.version == http::version::http_1_0) {
        response.fields.erase("Transfer-Encoding");
        response.trailers.clear();
        keep_alive = keep_alive && response.fields.contains("Content-Length");
      } else if (!response.fields.contains("Content-Length") && !response.fields.contains("Transfer-Encoding")) {
        response.fields.set("Transfer-Encoding", "chunked");
      }
    }

    prepare(response, head, keep_alive);

    if (in_pieces) {
      auto nothing_left = false;

      if (!co_await send_streamed(stream, response, head, keep_alive, options, body ? &*body : nullptr, nothing_left)) {
        // A writer reading the request found it at fault, before the response started.
        if (body && body->failure() && nothing_left) {
          co_return co_await refuse(*body->failure());
        }

        co_return connection_end::nothing_owed;
      }
    } else if (co_await send_response(stream, response, keep_alive, options)) {
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

  std::stop_source stop;
  auto const stop_lingering = [&stop] { stop.request_stop(); };
  std::stop_callback const on_stop{stop_token, stop_lingering};
  std::stop_callback const on_drain{options.drain, stop_lingering};

  (void)co_await async::detail::stop_scope<std::error_code>{
      async::with_timeout(options.lingering_time, drop_what_comes(stream, options.lingering_timeout)),
      stop.get_token()};
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

} // namespace

async::task<> serve_connection(io::any_stream stream, http::router const &router, connection_options const &options) {
  auto const end = co_await serve_requests(stream, router, options);

  co_await end_connection(stream, options, end);
}

async::task<> serve_connection(io::any_stream stream, http::router const &router) {
  static connection_options const defaults;

  return serve_connection(std::move(stream), router, defaults);
}

} // namespace quoll::server
