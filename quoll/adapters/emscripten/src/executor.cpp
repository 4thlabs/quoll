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
#include <quoll/emscripten/executor.hpp>

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/emscripten/detail/js.hpp>

#include <emscripten/emscripten.h>

#include <chrono>
#include <coroutine>
#include <optional>
#include <stop_token>

namespace quoll::emscripten {

namespace detail {

namespace {

// A setTimeout() awaited by a coroutine; a stop request clears it. Everything runs on the event loop's thread: the
// operation is not thread-safe.
//
// The completion points to the operation, in the frame of the suspended coroutine, which is destroyed only after it
// resumes. Movable until it suspends, so that it can be co_await-ed as a temporary.
class timer_operation {
public:
  explicit timer_operation(std::chrono::nanoseconds duration) noexcept : duration_(duration) {}

  timer_operation(timer_operation const &) = delete;
  timer_operation &operator=(timer_operation const &) = delete;
  timer_operation &operator=(timer_operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  timer_operation(timer_operation &&other) noexcept : duration_(other.duration_) {}

  ~timer_operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Returns false, resuming the awaiter at once, when stop is already requested.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      return false;
    }

    item_.handle = handle;
    context_ = &context;

    auto const milliseconds = std::chrono::duration<double, std::milli>{duration_}.count();

    stop_callback_.emplace(context.stop_token, canceller{quoll_emscripten_timer_start(milliseconds, &complete, this)});
    return true;
  }

  [[nodiscard]] bool await_resume() noexcept {
    stop_callback_.reset();
    return elapsed_;
  }

private:
  struct canceller {
    int id;

    void operator()() const noexcept { quoll_emscripten_timer_cancel(id); }
  };

  // Called by the event loop. This executor's post cannot fail (see async::Executor).
  static void complete(void *operation, int elapsed) noexcept {
    auto &self = *static_cast<timer_operation *>(operation);

    self.elapsed_ = elapsed != 0;
    self.context_->executor.post(self.item_);
  }

  std::chrono::nanoseconds duration_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  bool elapsed_ = false;
  std::optional<std::stop_callback<canceller>> stop_callback_;
};

static_assert(async::ContextAwaitable<timer_operation>);

} // namespace

} // namespace detail

std::chrono::nanoseconds executor::now() const noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double, std::milli>{::emscripten_get_now()}
  );
}

async::task<bool> executor::wait_for(std::chrono::nanoseconds duration) const {
  co_return co_await detail::timer_operation{duration};
}

http::platform_ref executor::query(async::service_tag<http::platform_ref>) const {
  return http::platform_ref{detail::the_fetch_platform};
}

} // namespace quoll::emscripten
