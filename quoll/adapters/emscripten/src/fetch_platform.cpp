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

#include <quoll/emscripten/detail/fetch_platform.hpp>
#include <quoll/http/detail/check.hpp>

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/emscripten/detail/js.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/http/verb.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::emscripten {

namespace detail {

namespace {

// Fields that fetch sets itself, and rejects or ignores when given.
constexpr std::array<std::string_view, 9> fetch_managed_fields{
    "Connection", "Content-Length", "Expect", "Host", "Keep-Alive", "TE", "Trailer", "Transfer-Encoding", "Upgrade",
};

// Fields of a response that describe the bytes on the wire, which fetch has already decoded (decompressed,
// de-chunked): they do not describe the body it hands over.
constexpr std::array<std::string_view, 3> fetch_decoded_fields{
    "Content-Encoding",
    "Content-Length",
    "Transfer-Encoding",
};

[[nodiscard]] bool is_one_of(std::string_view name, std::span<std::string_view const> names) noexcept {
  return std::ranges::any_of(names,
                             [name](std::string_view candidate) { return http::detail::iequals(name, candidate); });
}

// The fields of a request, one "name:value\n" line each, for quoll_emscripten_fetch_start. Checked as by
// http::serialize(): a name that is not a token, or a value with a line break, would split into other fields there.
[[nodiscard]] std::string pack_fields(http::fields const &fields) {
  std::string packed;

  for (auto const &entry : fields) {
    http::detail::check_field(entry);

    if (is_one_of(entry.name, fetch_managed_fields)) {
      continue;
    }

    packed.append(entry.name).append(":").append(entry.value).append("\n");
  }

  return packed;
}

// The JavaScript entry of a fetch, for as long as a body is to be read from it. Going, it aborts what is left of the
// fetch and frees the entry. Thread safety: none, on the event loop's thread.
class fetch_entry {
public:
  fetch_entry() noexcept = default;

  explicit fetch_entry(int id) noexcept : id_(id) {}

  fetch_entry(fetch_entry const &) = delete;
  fetch_entry &operator=(fetch_entry const &) = delete;

  fetch_entry(fetch_entry &&other) noexcept : id_(std::exchange(other.id_, 0)) {}

  fetch_entry &operator=(fetch_entry &&other) noexcept {
    if (this != &other) {
      reset();
      id_ = std::exchange(other.id_, 0);
    }

    return *this;
  }

  ~fetch_entry() { reset(); }

  // 0 once there is none: JavaScript counts from 1.
  [[nodiscard]] int id() const noexcept { return id_; }

  // Gives up on the fetch.
  void reset() noexcept {
    if (id_ != 0) {
      quoll_emscripten_fetch_abort(id_);
      finish();
    }
  }

  // The fetch is over: there is nothing to abort.
  void finish() noexcept {
    if (id_ != 0) {
      quoll_emscripten_fetch_release(std::exchange(id_, 0));
    }
  }

private:
  int id_ = 0;
};

// What the operations waiting on a fetch give a stop request to.
struct aborter {
  int id;

  void operator()() const noexcept { quoll_emscripten_fetch_abort(id); }
};

[[nodiscard]] std::string fetch_bytes(int id, fetch_part which) {
  std::string bytes(quoll_emscripten_fetch_size(id, static_cast<int>(which)), '\0');

  quoll_emscripten_fetch_copy(id, static_cast<int>(which), bytes.data());
  return bytes;
}

// What an aborted or failed fetch throws. `message`: why it failed, as JavaScript tells.
[[nodiscard]] std::system_error fetch_failure(fetch_outcome outcome, std::string const &url,
                                              std::string const &message) {
  if (outcome == fetch_outcome::aborted) {
    return std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::http::client: " + url};
  }

  return std::system_error{std::make_error_code(std::errc::io_error), "quoll::http::client: " + url + ": " + message};
}

// What a fetch_operation gives: its response, and what reads its body and the URL its
// failures name.
struct fetched {
  http::response response;
  fetch_entry entry;
  std::string url;
};

// A fetch() awaited by a coroutine. The completion, run by the event loop, reads the response out of JavaScript and
// posts the coroutine back to its executor. A stop request aborts the fetch, through its AbortController. Everything
// runs on the event loop's thread: the operation is not thread-safe.
//
// The completion points to the operation, in the frame of the suspended coroutine, which is destroyed only after it
// resumes. Movable until it suspends, so that it can be co_await-ed as a temporary.
class fetch_operation {
public:
  // `fields`: the request's fields, packed by pack_fields().
  fetch_operation(std::string url, std::string fields, http::request const &request)
      : url_(std::move(url)), fields_(std::move(fields)), request_(&request) {}

  fetch_operation(fetch_operation const &) = delete;
  fetch_operation &operator=(fetch_operation const &) = delete;
  fetch_operation &operator=(fetch_operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  fetch_operation(fetch_operation &&other) noexcept
      : url_(std::move(other.url_)), fields_(std::move(other.fields_)), request_(other.request_) {}

  ~fetch_operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Returns false, resuming the awaiter at once, when stop is already requested.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      outcome_ = fetch_outcome::aborted;
      return false;
    }

    auto const method = http::to_string(request_->method);

    item_.handle = handle;
    context_ = &context;

    auto const id =
        quoll_emscripten_fetch_start(url_.c_str(), method.data(), method.size(), fields_.data(), fields_.size(),
                                     request_->body.data(), request_->body.size(), &complete, this);

    stop_callback_.emplace(context.stop_token, aborter{id});
    return true;
  }

  [[nodiscard]] fetched await_resume() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }

    if (outcome_ != fetch_outcome::response) {
      throw fetch_failure(outcome_, url_, failure_);
    }

    return fetched{std::move(response_), std::move(entry_), std::move(url_)};
  }

private:
  // Called by the event loop once the fetch settled. A failure to post the coroutine back calls std::terminate (see
  // async::Executor); this executor's post cannot fail.
  static void complete(void *operation, int id, int outcome) noexcept {
    auto &self = *static_cast<fetch_operation *>(operation);

    // Settled: a stop from here on is no longer about this, and must not abort what has just succeeded, a body to come
    // included, in the time before what waits for it resumes.
    self.stop_callback_.reset();
    self.entry_ = fetch_entry{id};

    try {
      self.read(id, static_cast<fetch_outcome>(outcome));
    } catch (...) {
      self.exception_ = std::current_exception();
    }

    // Otherwise it stays for the body, unless the exception above leaves it to abort with this operation.
    if (self.outcome_ != fetch_outcome::response) {
      self.entry_.finish();
    }

    self.context_->executor.post(self.item_);
  }

  void read(int id, fetch_outcome outcome) {
    outcome_ = outcome;

    if (outcome != fetch_outcome::response) {
      failure_ = fetch_bytes(id, fetch_part::reason);
      return;
    }

    response_.status = static_cast<http::status>(quoll_emscripten_fetch_status(id));
    response_.reason = fetch_bytes(id, fetch_part::reason);

    auto const fields = fetch_bytes(id, fetch_part::fields);
    std::string_view rest{fields};

    while (!rest.empty()) {
      auto const line = rest.substr(0, rest.find('\n'));
      auto const colon = line.find(':');

      rest.remove_prefix(std::min(rest.size(), line.size() + 1));

      if (!is_one_of(line.substr(0, colon), fetch_decoded_fields)) {
        response_.fields.add(line.substr(0, colon), line.substr(colon + 1));
      }
    }
  }

  std::string url_;
  std::string fields_;
  http::request const *request_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  fetch_outcome outcome_ = fetch_outcome::response;
  http::response response_;
  fetch_entry entry_;
  std::string failure_;
  std::exception_ptr exception_;
  std::optional<std::stop_callback<aborter>> stop_callback_;
};

static_assert(async::ContextAwaitable<fetch_operation>);

// A JavaScript read of the next piece of a body, into `piece`, awaited by a coroutine: true for a piece, false at the
// end of the body. As fetch_operation: a stop request aborts the fetch, and the completion points to the operation.
class read_operation {
public:
  // `url`, for what it throws, and `piece` outlive it.
  read_operation(int id, std::string const &url QUOLL_LIFETIMEBOUND, std::string &piece QUOLL_LIFETIMEBOUND) noexcept
      : id_(id), url_(&url), piece_(&piece) {}

  read_operation(read_operation const &) = delete;
  read_operation &operator=(read_operation const &) = delete;
  read_operation &operator=(read_operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  read_operation(read_operation &&other) noexcept : id_(other.id_), url_(other.url_), piece_(other.piece_) {}

  ~read_operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Returns false, resuming the awaiter at once, when stop is already requested.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      outcome_ = fetch_outcome::aborted;
      return false;
    }

    item_.handle = handle;
    context_ = &context;
    quoll_emscripten_fetch_read(id_, &complete, this);
    stop_callback_.emplace(context.stop_token, aborter{id_});
    return true;
  }

  [[nodiscard]] bool await_resume() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }

    if (outcome_ == fetch_outcome::aborted || outcome_ == fetch_outcome::failed) {
      throw fetch_failure(outcome_, *url_, failure_);
    }

    return outcome_ == fetch_outcome::response;
  }

private:
  // Called by the event loop once the read settled. See fetch_operation::complete().
  static void complete(void *operation, int outcome) noexcept {
    auto &self = *static_cast<read_operation *>(operation);

    // See fetch_operation::complete().
    self.stop_callback_.reset();
    self.outcome_ = static_cast<fetch_outcome>(outcome);

    try {
      self.read();
    } catch (...) {
      self.exception_ = std::current_exception();
    }

    self.context_->executor.post(self.item_);
  }

  void read() {
    if (outcome_ == fetch_outcome::aborted || outcome_ == fetch_outcome::failed) {
      failure_ = fetch_bytes(id_, fetch_part::reason);
    } else if (outcome_ == fetch_outcome::response) {
      piece_->resize(quoll_emscripten_fetch_size(id_, static_cast<int>(fetch_part::body)));
      quoll_emscripten_fetch_copy(id_, static_cast<int>(fetch_part::body), piece_->data());
    }
  }

  int id_;
  std::string const *url_;
  std::string *piece_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  fetch_outcome outcome_ = fetch_outcome::end;
  std::string failure_;
  std::exception_ptr exception_;
  std::optional<std::stop_callback<aborter>> stop_callback_;
};

static_assert(async::ContextAwaitable<read_operation>);

// The body of a fetch being read in pieces, and its entry: what a streamed_response holds. Thread safety: none, on the
// event loop's thread.
class fetch_stream {
public:
  fetch_stream(fetch_entry entry, std::string url) : entry_(std::move(entry)), url_(std::move(url)) {
    // Nothing will come, whether the caller reads or not: JavaScript has no reason to keep it.
    if (quoll_emscripten_fetch_has_body(entry_.id()) == 0) {
      entry_.finish();
    }
  }

  [[nodiscard]] async::task<std::optional<std::string_view>> read() QUOLL_LIFETIMEBOUND {
    if (failure_) {
      std::rethrow_exception(failure_);
    }

    if (entry_.id() == 0) {
      co_return std::nullopt;
    }

    try {
      if (co_await read_operation{entry_.id(), url_, piece_}) {
        co_return std::string_view{piece_};
      }
    } catch (...) {
      // A cancellation too: a stream does not take it up again.
      failure_ = std::current_exception();
      entry_.reset();
      throw;
    }

    entry_.finish();
    co_return std::nullopt;
  }

private:
  fetch_entry entry_;
  std::string url_;
  std::string piece_;
  // What ended the reading, for the following reads to throw too.
  std::exception_ptr failure_;
};

// `fields`: the request's fields, packed by pack_fields().
[[nodiscard]] async::task<http::streamed_response> fetch_in_pieces(std::string url, std::string fields,
                                                                   http::request request) {
  auto result = co_await fetch_operation{std::move(url), std::move(fields), request};
  auto stream = std::make_shared<fetch_stream>(std::move(result.entry), std::move(result.url));
  auto &response = result.response;

  co_return http::streamed_response{response.status,
                                    std::move(response.reason),
                                    response.version,
                                    std::move(response.fields),
                                    http::response_body{stream},
                                    std::make_shared<http::fields const>()};
}

} // namespace

async::task<http::streamed_response> fetch_platform::stream(std::string url, http::request request) const {
  auto fields = detail::pack_fields(request.fields);

  return detail::fetch_in_pieces(std::move(url), std::move(fields), std::move(request));
}

} // namespace detail

} // namespace quoll::emscripten
