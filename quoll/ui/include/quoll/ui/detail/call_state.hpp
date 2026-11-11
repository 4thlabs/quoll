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

#ifndef QUOLL_UI_DETAIL_CALL_STATE_HPP
#define QUOLL_UI_DETAIL_CALL_STATE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace quoll::ui::detail {

// A call of a callback, between the task of the component that wrote it, which does the work, and the caller, which
// waits for its end, or stops waiting.
//
// Thread safety: none, as its app: both sides run on the app's executor.
template <class R>
struct call_state {
  bool done = false;
  std::optional<async::detail::non_void_t<R>> value{};
  std::exception_ptr failure{};
  // What waits, while it does.
  async::work_item *waiter = nullptr;
  std::optional<async::executor_ref> executor{};
  async::detail::chain_owner *chain = nullptr;

  // The work has ended: wakes the caller, if it still waits. False when nothing waits. A post that throws ends the
  // program, as nothing would be left to resume the caller.
  bool end() noexcept {
    done = true;

    if (waiter == nullptr) {
      return false;
    }

    executor->post(*std::exchange(waiter, nullptr));
    return true;
  }

  // The work will not end, stopped: the caller ends on the stop too, if it still waits.
  void abandon() noexcept {
    if (waiter == nullptr) {
      return;
    }

    waiter = nullptr;
    async::detail::stop_chain(chain);
  }
};

// Ends the work's side of a call: what the work gave, or, destroyed without either, as the work ends on a stop, the
// stop.
template <class R>
class call_end {
public:
  explicit call_end(std::shared_ptr<call_state<R>> state) noexcept : state_(std::move(state)) {}

  call_end(call_end const &) = delete;
  call_end(call_end &&) = delete;
  call_end &operator=(call_end const &) = delete;
  call_end &operator=(call_end &&) = delete;

  ~call_end() {
    if (!state_->done) {
      state_->abandon();
    }
  }

  void give(async::detail::non_void_t<R> value) {
    state_->value.emplace(std::move(value));
    state_->end();
  }

  // False when nothing waits for `failure`, which the work then throws again, for its app to report.
  [[nodiscard]] bool fail(std::exception_ptr failure) noexcept {
    state_->failure = std::move(failure);
    return state_->end();
  }

private:
  std::shared_ptr<call_state<R>> state_;
};

// The caller's side: waits for the work to end, and gives what it gave. Ends on the caller's stop, which leaves the
// work running.
template <class R>
class [[nodiscard]] call_wait {
public:
  explicit call_wait(std::shared_ptr<call_state<R>> state) noexcept : state_(std::move(state)) {}

  call_wait(call_wait const &) = delete;

  // Before it waits only, as an awaitable is moved into what awaits it: nothing is registered yet.
  call_wait(call_wait &&other) noexcept : state_(std::move(other.state_)) {}

  call_wait &operator=(call_wait const &) = delete;
  call_wait &operator=(call_wait &&) = delete;

  // Destroyed with its frame, while it waits: the work no longer wakes it.
  ~call_wait() {
    if (state_ && state_->waiter == &item_) {
      state_->waiter = nullptr;
    }
  }

  [[nodiscard]] bool await_ready() const noexcept { return state_->done; }

  // Touches nothing after stop_chain(), which may destroy this frame.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      async::detail::stop_chain(context.owner);
      return true;
    }

    item_.handle = handle;
    state_->waiter = &item_;
    state_->executor.emplace(context.executor);
    state_->chain = context.owner;
    on_stop_.emplace(context.stop_token, stopper{this});
    return true;
  }

  async::detail::non_void_t<R> await_resume() {
    on_stop_.reset();

    if (state_->failure) {
      std::rethrow_exception(state_->failure);
    }

    return std::move(*state_->value);
  }

private:
  struct stopper {
    call_wait *self;

    // The caller stops waiting; the work goes on. Touches nothing after stop_chain().
    void operator()() const noexcept {
      auto &state = *self->state_;

      if (state.waiter != &self->item_) {
        return;
      }

      state.waiter = nullptr;
      async::detail::stop_chain(state.chain);
    }
  };

  std::shared_ptr<call_state<R>> state_;
  async::work_item item_{};
  std::optional<std::stop_callback<stopper>> on_stop_{};
};

static_assert(async::ContextAwaitable<call_wait<void>>);

// A task giving `value` at once.
template <class T>
[[nodiscard]] async::task<T> ready(T value) {
  co_return value;
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CALL_STATE_HPP
