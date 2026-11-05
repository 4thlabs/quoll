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

#include <quoll/client/transport.hpp>

#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/connection_field.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/serializer.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/write.hpp>

#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace quoll::client::detail {

namespace {

// The idle connections of a transport, between two requests. Thread safety: take() and give_back() may be called
// from any thread, concurrently. The lock serves the requests of one transport sent from several threads (a thread
// pool); with a single thread, as under WASM, it is never contended, and std::mutex needs no thread support there.
class connection_pool {
public:
  explicit connection_pool(std::size_t capacity) : capacity_(capacity) {}

  // The connection used last, the least likely to have been closed by the server for being idle.
  [[nodiscard]] std::optional<io::any_stream> take() {
    return idle_.with([](std::vector<io::any_stream> &idle) -> std::optional<io::any_stream> {
      if (idle.empty()) {
        return std::nullopt;
      }

      auto connection = std::move(idle.back());

      idle.pop_back();
      return connection;
    });
  }

  // Keeps `connection` for a next request, or closes it if the pool is full.
  void give_back(io::any_stream connection) {
    idle_.with([&](std::vector<io::any_stream> &idle) {
      if (idle.size() < capacity_) {
        idle.push_back(std::move(connection));
      }
    });
  }

  // Closes the idle connections past `count`, the oldest first, past the lock.
  void keep_at_most(std::size_t count) {
    auto const closed = idle_.with([count](std::vector<io::any_stream> &idle) {
      std::vector<io::any_stream> past_it;

      if (idle.size() > count) {
        auto const end = idle.begin() + static_cast<std::ptrdiff_t>(idle.size() - count);

        past_it.assign(std::make_move_iterator(idle.begin()), std::make_move_iterator(end));
        idle.erase(idle.begin(), end);
      }

      return past_it;
    });
  }

  // Closes the idle connections, past the lock.
  void clear() {
    auto const closed = idle_.with([](std::vector<io::any_stream> &idle) { return std::exchange(idle, {}); });
  }

private:
  async::detail::synchronized<std::vector<io::any_stream>> idle_;
  std::size_t capacity_;
};

// A request serialized by send(), before its task runs, with what the exchange needs to know about it.
struct prepared_request {
  std::string wire;
  http::verb method = http::verb::get;
  // The request lets its connection carry another one (HTTP/1.1, no `Connection: close`).
  bool keep_alive = false;
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

// Interim responses read before the final one, against a server sending them forever.
constexpr int max_interim_responses = 8;

// How a connection kept from an earlier request shows that the server closed it meanwhile.
[[nodiscard]] bool closed_meanwhile(std::error_code error) noexcept {
  return error == io::error::end_of_stream || error == std::errc::connection_reset || error == std::errc::broken_pipe;
}

// Sends `request` on `connection` and reads its final response, skipping the interim 1xx ones; the connection goes
// back to `pool` if it can carry another request. With `retry_if_closed` (an idle connection), a connection found
// closed before any response, or answering 408 first (sent by the server before it closed the idle connection), is
// not an error: no response, the caller sends the request again on a new connection. Without it, a response or an
// exception.
async::task<std::optional<http::response>> exchange(connection_pool &pool, http::parse_limits limits,
                                                    io::any_stream connection, prepared_request const &request,
                                                    bool retry_if_closed) {
  auto const sent = co_await io::write(connection, std::as_bytes(std::span{request.wire}));

  if (sent.error) {
    if (retry_if_closed && closed_meanwhile(sent.error)) {
      co_return std::nullopt;
    }

    throw std::system_error{sent.error, "quoll::client: cannot send the request"};
  }

  std::string buffer;
  http::response_parser parser{limits};
  auto interim = 0;

  parser.set_request_method(request.method);

  for (;;) {
    if (auto const error = co_await http::read(connection, buffer, parser)) {
      if (retry_if_closed && interim == 0 && closed_meanwhile(error)) {
        co_return std::nullopt;
      }

      throw std::system_error{error, "quoll::client: cannot read the response"};
    }

    auto const keep_alive = parser.keep_alive();
    auto const upgrade = parser.upgrade();
    auto response = parser.release();

    if (http::is_informational(response.status) && !upgrade) {
      if (++interim > max_interim_responses) {
        throw std::system_error{http::error::bad_message, "quoll::client: too many interim responses"};
      }

      continue;
    }

    if (retry_if_closed && interim == 0 && response.status == http::status::request_timeout) {
      co_return std::nullopt;
    }

    // Bytes past the response would be read as the next one: a connection carrying them is not reused.
    if (request.keep_alive && keep_alive && !upgrade && buffer.empty()) {
      pool.give_back(std::move(connection));
    }

    co_return response;
  }
}

} // namespace

// What the copies of a transport share.
class transport_core {
public:
  transport_core(connector_handle connector_, std::string host_, std::uint16_t port_, bool secure_,
                 std::string host_field_, std::string prefix_, transport_options const &options_)
      : connector(std::move(connector_)), host(std::move(host_)), port(port_), secure(secure_),
        host_field(std::move(host_field_)), prefix(std::move(prefix_)), options(options_),
        pool(options_.max_idle_connections) {
    if (options_.max_connections > 0) {
      places.emplace(options_.max_connections);
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
  transport_options options;
  connection_pool pool;
  // Requests on a connection at once, when options.max_connections limits them.
  std::optional<async::detail::slots> places;
};

namespace {

// A place held for as long as a request is on a connection.
class held_place {
public:
  held_place() noexcept = default;

  held_place(held_place const &) = delete;
  held_place &operator=(held_place const &) = delete;
  held_place(held_place &&) = delete;
  held_place &operator=(held_place &&) = delete;

  ~held_place() { release(); }

  void release() noexcept {
    if (places_ != nullptr) {
      std::exchange(places_, nullptr)->release();
    }
  }

  // Owned from the moment it is given, whatever fails around the wait for it.
  async::task<> take(async::detail::slots &places) {
    co_await places.acquire();
    places_ = &places;
  }

private:
  async::detail::slots *places_ = nullptr;
};

// A new connection is about to open: what is idle past the limit makes room for it.
void make_room(transport_core &shared) {
  if (shared.places) {
    shared.pool.keep_at_most(shared.places->available());
  }
}

// The task holds the shared state, not the transport.
async::task<http::response> send_prepared(std::shared_ptr<transport_core> shared, prepared_request request) {
  held_place place;

  if (shared->places) {
    co_await async::detail::with_optional_timeout(shared->options.connect_timeout, place.take(*shared->places));
  }

  if (idempotent(request.method)) {
    if (auto idle = shared->pool.take()) {
      auto response = co_await async::detail::with_optional_timeout(
          shared->options.response_timeout,
          exchange(shared->pool, shared->options.limits, std::move(*idle), request, true));

      if (response) {
        co_return std::move(*response);
      }

      // Closed by the server after some idle time: the connections idle for longer are likely closed as well.
      shared->pool.clear();
    }
  }

  make_room(*shared);

  auto connection = co_await async::detail::with_optional_timeout(shared->options.connect_timeout, shared->open());
  auto response = co_await async::detail::with_optional_timeout(
      shared->options.response_timeout,
      exchange(shared->pool, shared->options.limits, std::move(connection), request, false));

  // Without retry_if_closed, exchange() produces a response or throws.
  assert(response && "client::exchange() without retry_if_closed produced no response");
  co_return std::move(*response);
}

// The request as it goes on the wire. Throws std::invalid_argument for what http::serialize() rejects.
[[nodiscard]] prepared_request prepare(transport_core const &shared, http::request const &request) {
  auto const keep_alive = request.version == http::version::http_1_1 &&
                          !http::detail::closes_connection(request.fields) && shared.options.max_idle_connections > 0;

  return prepared_request{http::serialize(request), request.method, keep_alive};
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
async::task<http::response> send_following(std::shared_ptr<transport_core> shared, http::request request,
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
      throw std::system_error{http::error::too_many_redirects, "quoll::client: too many redirects"};
    }

    redirect_request(request, response.status, std::move(*target));
    prepared = prepare(*shared, request);
  }
}

// A response being read in pieces, and its connection: what a streamed_response holds. It owns its place in
// max_connections until the body has come to its end, or until it goes. Thread safety: none, on the loop of what reads
// it; what it shares with its transport has its own.
class response_stream {
public:
  explicit response_stream(std::shared_ptr<transport_core> shared)
      : shared_(std::move(shared)), parser_(shared_->options.limits), trailers_(std::make_shared<http::fields>()) {}

  [[nodiscard]] held_place &place() noexcept { return place_; }

  // Sends `request` on `connection` and reads the head of its final response. As in exchange(): with `retry_if_closed`,
  // a connection found closed, or answering 408 first, gives false, and the caller goes on with another.
  [[nodiscard]] async::task<bool> start(io::any_stream connection, prepared_request const &request,
                                        bool retry_if_closed) {
    auto const sent = co_await io::write(connection, std::as_bytes(std::span{request.wire}));

    if (sent.error) {
      if (retry_if_closed && closed_meanwhile(sent.error)) {
        co_return false;
      }

      throw std::system_error{sent.error, "quoll::client: cannot send the request"};
    }

    auto interim = 0;

    // Afresh for each attempt: the one before may have left anything in it.
    buffer_.clear();
    parser_ = http::response_parser{shared_->options.limits};
    parser_.set_request_method(request.method);
    // An interim response is read whole: only the final one is the caller's to read.
    parser_.set_body_streaming([](http::response const &head) { return !http::is_informational(head.status); });

    for (;;) {
      if (auto const error = co_await http::read_head(connection, buffer_, parser_)) {
        if (retry_if_closed && interim == 0 && closed_meanwhile(error)) {
          co_return false;
        }

        throw std::system_error{error, "quoll::client: cannot read the response"};
      }

      auto const status = parser_.head().status;

      if (http::is_informational(status) && !parser_.upgrade()) {
        if (++interim > max_interim_responses) {
          throw std::system_error{http::error::bad_message, "quoll::client: too many interim responses"};
        }

        if (!parser_.done()) {
          if (auto const error = co_await http::read(connection, buffer_, parser_)) {
            throw std::system_error{error, "quoll::client: cannot read the response"};
          }
        }

        // The request method holds until the final one.
        static_cast<void>(parser_.release());
        continue;
      }

      if (retry_if_closed && interim == 0 && status == http::status::request_timeout) {
        co_return false;
      }

      connection_.emplace(std::move(connection));
      request_keeps_alive_ = request.keep_alive;
      co_return true;
    }
  }

  // What the caller gets of the head: the rest of the message stays for read(). A message with nothing more to come
  // gives its connection back at once, as nothing will read it to its end.
  [[nodiscard]] streamed_response head(std::shared_ptr<response_stream> const &self) {
    auto const &message = parser_.head();
    streamed_response response{
        message.status, message.reason, message.version, message.fields, response_body{http::body_reader{*this}, self},
        trailers_};

    // Nothing more is due: its connection and its place go now, whether the caller reads what came or not.
    if (parser_.done()) {
      parser_.take_body(piece_);
      held_piece_ = !piece_.empty();
      finish();
    } else if (auto const failure = parser_.failure(); failure && message.body.empty()) {
      fail(failure);
    }

    return response;
  }

  [[nodiscard]] async::task<std::optional<std::string_view>> read() {
    if (held_piece_) {
      held_piece_ = false;
      co_return std::string_view{piece_};
    }

    if (failure_) {
      throw std::system_error{failure_, "quoll::client: cannot read the response"};
    }

    if (ended_) {
      co_return std::nullopt;
    }

    // What a read cut short leaves is no new piece.
    piece_.clear();

    std::error_code failed;

    try {
      co_await async::detail::with_optional_timeout(shared_->options.response_timeout, read_piece());
    } catch (std::system_error const &thrown) {
      failed = thrown.code();
    } catch (...) {
      // What it had read may be lost: nothing can follow it.
      fail(std::make_error_code(std::errc::io_error));
      throw;
    }

    // A cancellation too: a stream does not take it up again, and over TLS its session is gone.
    if (failed) {
      fail(failed);
      throw std::system_error{failed, "quoll::client: cannot read the response"};
    }

    // At once, even with a last piece to hand over: its connection and its place are done with, for as long as the
    // caller is at work on that piece. What it leaves alone is the piece.
    if (parser_.done()) {
      finish();
    } else if (auto const failure = parser_.failure()) {
      // Past this piece: the connection can carry nothing more, and the next read says so.
      fail(failure);
    }

    if (piece_.empty()) {
      co_return std::nullopt;
    }

    co_return std::string_view{piece_};
  }

private:
  // Thrown rather than returned: a read a timeout cancelled is then that timeout's timed_out.
  [[nodiscard]] async::task<> read_piece() {
    if (auto const error = co_await http::read_body(*connection_, buffer_, parser_, piece_)) {
      throw std::system_error{error, "quoll::client: cannot read the response"};
    }
  }

  // It goes now rather than with what holds this: the place is for others, and the server has no more to send.
  void fail(std::error_code why) noexcept {
    failure_ = why;
    connection_.reset();
    place_.release();
  }

  void finish() {
    ended_ = true;

    auto const keep_alive = parser_.keep_alive();
    auto const upgrade = parser_.upgrade();

    *trailers_ = parser_.release().trailers;

    // Bytes past the response would be read as the next one: a connection carrying them is not reused.
    if (request_keeps_alive_ && keep_alive && !upgrade && buffer_.empty()) {
      shared_->pool.give_back(std::move(*connection_));
    }

    connection_.reset();
    place_.release();
  }

  std::shared_ptr<transport_core> shared_;
  http::response_parser parser_;
  std::shared_ptr<http::fields> trailers_;
  // Before the connection: it closes first, and only then is its place another's.
  held_place place_;
  std::optional<io::any_stream> connection_;
  std::string buffer_;
  std::string piece_;
  std::error_code failure_;
  bool request_keeps_alive_ = false;
  bool ended_ = false;
  // What came whole with the head, for the first read.
  bool held_piece_ = false;
};

async::task<streamed_response> stream_prepared(std::shared_ptr<transport_core> shared, prepared_request request) {
  auto stream = std::make_shared<response_stream>(shared);

  if (shared->places) {
    co_await async::detail::with_optional_timeout(shared->options.connect_timeout,
                                                  stream->place().take(*shared->places));
  }

  if (idempotent(request.method)) {
    if (auto idle = shared->pool.take()) {
      auto const started = co_await async::detail::with_optional_timeout(
          shared->options.response_timeout, stream->start(std::move(*idle), request, true));

      if (started) {
        co_return stream->head(stream);
      }

      // Closed by the server after some idle time: the connections idle for longer are likely closed as well.
      shared->pool.clear();
    }
  }

  make_room(*shared);

  auto connection = co_await async::detail::with_optional_timeout(shared->options.connect_timeout, shared->open());

  static_cast<void>(co_await async::detail::with_optional_timeout(
      shared->options.response_timeout, stream->start(std::move(connection), request, false)));
  co_return stream->head(stream);
}

} // namespace

std::shared_ptr<transport_core> make_transport_core(connector_handle connector, std::string host, std::uint16_t port,
                                                    bool secure, std::string prefix, transport_options const &options) {
  auto field = host_field(host, port, secure ? 443 : 80);

  return std::make_shared<transport_core>(std::move(connector), std::move(host), port, secure, std::move(field),
                                          std::move(prefix), options);
}

namespace {

// What send() and stream() ask of a request before anything runs.
void make_ready(transport_core const &core, http::request &request) {
  if (!request.target.starts_with('/')) {
    throw std::invalid_argument{"quoll::client::transport: the target is not in origin form"};
  }

  request.target.insert(0, core.prefix);

  if (!request.fields.contains("Host")) {
    request.fields.add("Host", core.host_field);
  }
}

} // namespace

async::task<streamed_response> stream(std::shared_ptr<transport_core> const &core, http::request request) {
  make_ready(*core, request);
  return stream_prepared(core, prepare(*core, request));
}

async::task<http::response> send(std::shared_ptr<transport_core> const &core, http::request request) {
  make_ready(*core, request);

  auto prepared = prepare(*core, request);

  if (core->options.max_redirects == 0) {
    return send_prepared(core, std::move(prepared));
  }

  return send_following(core, std::move(request), std::move(prepared));
}

} // namespace quoll::client::detail
