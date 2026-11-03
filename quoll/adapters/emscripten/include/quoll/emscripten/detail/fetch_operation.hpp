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

#ifndef QUOLL_EMSCRIPTEN_DETAIL_FETCH_OPERATION_HPP
#define QUOLL_EMSCRIPTEN_DETAIL_FETCH_OPERATION_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/emscripten/detail/js.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::emscripten::detail {

// Fields that fetch sets itself, and rejects or ignores when given.
inline constexpr std::array<std::string_view, 9> fetch_managed_fields{
    "Connection", "Content-Length", "Expect", "Host", "Keep-Alive", "TE", "Trailer", "Transfer-Encoding", "Upgrade",
};

// The fields of a request, one "name:value\n" line each, for quoll_emscripten_fetch_start.
[[nodiscard]] inline std::string pack_fields(http::fields const &fields) {
  std::string packed;

  for (auto const &entry : fields) {
    auto const managed = std::ranges::any_of(
        fetch_managed_fields, [&entry](std::string_view name) { return http::detail::iequals(entry.name, name); });

    if (managed) {
      continue;
    }

    packed.append(entry.name).append(":").append(entry.value).append("\n");
  }

  return packed;
}

// A fetch() awaited by a coroutine. The completion, run by the event loop, reads the response out of JavaScript and
// posts the coroutine back to its executor. A stop request aborts the fetch, through its AbortController. Everything
// runs on the event loop's thread: the operation is not thread-safe.
//
// The completion points to the operation, in the frame of the suspended coroutine, which is destroyed only after it
// resumes. Movable until it suspends, so that it can be co_await-ed as a temporary.
class fetch_operation {
public:
  fetch_operation(std::string url, http::request const &request) : url_(std::move(url)), request_(&request) {}

  fetch_operation(fetch_operation const &) = delete;
  fetch_operation &operator=(fetch_operation const &) = delete;
  fetch_operation &operator=(fetch_operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  fetch_operation(fetch_operation &&other) noexcept : url_(std::move(other.url_)), request_(other.request_) {}

  ~fetch_operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Returns false, resuming the awaiter at once, when stop is already requested.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      outcome_ = fetch_outcome::aborted;
      return false;
    }

    std::string const method{http::to_string(request_->method)};
    auto const fields = pack_fields(request_->fields);

    item_.handle = handle;
    context_ = &context;
    auto const id = quoll_emscripten_fetch_start(url_.c_str(), method.c_str(), fields.c_str(), request_->body.data(),
                                                 request_->body.size(), &complete, this);

    stop_callback_.emplace(context.stop_token, aborter{id});
    return true;
  }

  [[nodiscard]] http::response await_resume() {
    stop_callback_.reset();

    if (exception_) {
      std::rethrow_exception(exception_);
    }

    switch (outcome_) {
    case fetch_outcome::response:
      return std::move(response_);
    case fetch_outcome::aborted:
      throw std::system_error{std::make_error_code(std::errc::operation_canceled),
                              "quoll::emscripten::fetch_transport: " + url_};
    case fetch_outcome::failed:
      break;
    }

    throw std::system_error{std::make_error_code(std::errc::io_error),
                            "quoll::emscripten::fetch_transport: " + url_ + ": " + failure_};
  }

private:
  struct aborter {
    int id;

    void operator()() const noexcept { quoll_emscripten_fetch_abort(id); }
  };

  // Called by the event loop once the fetch settled. A failure to post the coroutine back calls std::terminate (see
  // async::Executor); this executor's post cannot fail.
  static void complete(void *operation, int id, int outcome) noexcept {
    auto &self = *static_cast<fetch_operation *>(operation);

    try {
      self.read(id, static_cast<fetch_outcome>(outcome));
    } catch (...) {
      self.exception_ = std::current_exception();
    }

    quoll_emscripten_fetch_release(id);
    self.context_->executor.post(self.item_);
  }

  void read(int id, fetch_outcome outcome) {
    outcome_ = outcome;

    if (outcome != fetch_outcome::response) {
      failure_ = part(id, fetch_part::reason);
      return;
    }

    response_.status = static_cast<http::status>(quoll_emscripten_fetch_status(id));
    response_.reason = part(id, fetch_part::reason);
    response_.body = part(id, fetch_part::body);

    auto const fields = part(id, fetch_part::fields);
    std::string_view rest{fields};

    while (!rest.empty()) {
      auto const line = rest.substr(0, rest.find('\n'));
      auto const colon = line.find(':');

      rest.remove_prefix(std::min(rest.size(), line.size() + 1));
      response_.fields.add(line.substr(0, colon), line.substr(colon + 1));
    }
  }

  [[nodiscard]] static std::string part(int id, fetch_part which) {
    std::string bytes(quoll_emscripten_fetch_size(id, static_cast<int>(which)), '\0');

    quoll_emscripten_fetch_copy(id, static_cast<int>(which), bytes.data());
    return bytes;
  }

  std::string url_;
  http::request const *request_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  fetch_outcome outcome_ = fetch_outcome::response;
  http::response response_;
  std::string failure_;
  std::exception_ptr exception_;
  std::optional<std::stop_callback<aborter>> stop_callback_;
};

static_assert(async::ContextAwaitable<fetch_operation>);

} // namespace quoll::emscripten::detail

#endif // QUOLL_EMSCRIPTEN_DETAIL_FETCH_OPERATION_HPP
