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

#ifndef QUOLL_UI_DETAIL_COMPONENT_CALL_STATE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_CALL_STATE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <tuple>
#include <utility>

namespace quoll::ui::detail {

// A call of a callback, between the task of the component that wrote it, which does the work, and the caller, which
// waits for its end, or stops waiting. The work may end before the caller waits, and the caller's stop may come from
// any thread: what both sides read is under one lock.
//
// Thread safety: any thread, each side once.
template <class R>
class call_state {
public:
  // The work's side. What it gave: wakes the caller, if it waits.
  void give(async::detail::non_void_t<R> value) {
    end([&](shared &state) { state.value.emplace(std::move(value)); });
  }

  // What it threw: false when the caller no longer waits, which the work then throws again, for its app to report.
  [[nodiscard]] bool fail(std::exception_ptr failure) noexcept {
    auto seen = false;

    end([&](shared &state) noexcept {
      state.failure = std::move(failure);
      seen = !state.gone;
    });

    return seen;
  }

  // It ended on a stop, giving nothing: the caller ends on the stop too, now if it waits, or as it comes to.
  void abandon() noexcept {
    auto *const chain = shared_.with([](shared &state) noexcept -> async::detail::chain_owner * {
      state.stopped = true;

      if (state.waiter == nullptr) {
        return nullptr;
      }

      state.waiter = nullptr;
      state.gone = true;
      return state.chain;
    });

    if (chain != nullptr) {
      async::detail::stop_chain(chain);
    }
  }

  // The caller's side. Whether the work has given or thrown already.
  [[nodiscard]] bool ready() const noexcept {
    return shared_.with([](shared const &state) noexcept { return state.done && !state.stopped; });
  }

  // Waits on `item`, unless the work ended meanwhile, or the caller's stop came: then false, and the caller goes on,
  // or, the work or the caller stopped, `stopped` is set and the caller ends on the stop. Checked under the lock, after
  // the stop callback is registered, so that a stop coming at any time is seen here or by it.
  [[nodiscard]] bool wait(async::work_item &item, async::executor_ref executor, async::detail::chain_owner *chain,
                          std::stop_token const &caller_stop, bool &stopped) {
    return shared_.with([&](shared &state) {
      // What the work gave or threw comes first: a failure the caller takes is not lost.
      stopped = state.stopped || (!state.done && caller_stop.stop_requested());

      if (state.done || stopped) {
        state.gone = stopped;
        return false;
      }

      state.waiter = &item;
      state.executor.emplace(executor);
      state.chain = chain;
      return true;
    });
  }

  // The caller stops waiting on `item`: the chain to stop, if it still waited.
  [[nodiscard]] async::detail::chain_owner *stop_waiting(async::work_item const &item) noexcept {
    return shared_.with([&](shared &state) noexcept -> async::detail::chain_owner * {
      if (state.waiter != &item) {
        return nullptr;
      }

      state.waiter = nullptr;
      state.gone = true;
      return state.chain;
    });
  }

  // What the work gave, or what it threw, thrown here.
  [[nodiscard]] async::detail::non_void_t<R> take() {
    auto [value, failure] =
        shared_.with([](shared &state) { return std::pair{std::move(state.value), std::move(state.failure)}; });

    if (failure) {
      std::rethrow_exception(failure);
    }

    return std::move(*value);
  }

private:
  struct shared {
    bool done = false;
    bool stopped = false;
    // The caller no longer waits: it stopped, or the work's stop ended it.
    bool gone = false;
    std::optional<async::detail::non_void_t<R>> value{};
    std::exception_ptr failure{};
    async::work_item *waiter = nullptr;
    std::optional<async::executor_ref> executor{};
    async::detail::chain_owner *chain = nullptr;
  };

  // Records the end under the lock, then posts the caller, if it waits, past it. A post that throws ends the program,
  // as nothing would be left to resume the caller.
  template <class F>
  void end(F record) noexcept(noexcept(record(std::declval<shared &>()))) {
    auto const woken =
        shared_.with([&](shared &state) -> std::optional<std::pair<async::work_item *, async::executor_ref>> {
          record(state);
          state.done = true;

          if (state.waiter == nullptr) {
            return std::nullopt;
          }

          return std::pair{std::exchange(state.waiter, nullptr), *state.executor};
        });

    if (woken) {
      [&]() noexcept { woken->second.post(*woken->first); }();
    }
  }

  async::detail::synchronized<shared> shared_{};
};

// Ends the work's side of a call: what the work gave, or, destroyed without either, as the work ends on a stop, the
// stop. Thread safety: none, as the work's frame it lives in.
template <class R>
class call_end {
public:
  explicit call_end(std::shared_ptr<call_state<R>> state) noexcept : state_(std::move(state)) {}

  call_end(call_end const &) = delete;
  call_end(call_end &&) = delete;
  call_end &operator=(call_end const &) = delete;
  call_end &operator=(call_end &&) = delete;

  ~call_end() {
    if (!ended_) {
      state_->abandon();
    }
  }

  void give(async::detail::non_void_t<R> value) {
    state_->give(std::move(value));
    ended_ = true;
  }

  // False when nothing waits for `failure`: see call_state::fail.
  [[nodiscard]] bool fail(std::exception_ptr failure) noexcept {
    ended_ = true;
    return state_->fail(std::move(failure));
  }

private:
  std::shared_ptr<call_state<R>> state_;
  bool ended_ = false;
};

// The caller's side: waits for the work to end, and gives what it gave. Ends on the caller's stop, which leaves the
// work running. Thread safety: on the caller's executor, its stop from any thread, through call_state.
template <class R>
class [[nodiscard]] call_wait {
public:
  explicit call_wait(std::shared_ptr<call_state<R>> state) noexcept : state_(std::move(state)) {}

  call_wait(call_wait const &) = delete;

  // Before it waits only, as an awaitable is moved into what awaits it: nothing is registered yet.
  call_wait(call_wait &&other) noexcept : state_(std::move(other.state_)) {}

  call_wait &operator=(call_wait const &) = delete;
  call_wait &operator=(call_wait &&) = delete;

  // Destroyed with its frame while it waits, the stop callback first: the work no longer wakes it.
  ~call_wait() {
    on_stop_.reset();

    if (state_) {
      std::ignore = state_->stop_waiting(item_);
    }
  }

  [[nodiscard]] bool await_ready() const noexcept { return state_->ready(); }

  // Touches nothing after stop_chain(), which may destroy this frame.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    auto stopped = false;

    // The stop callback before the wait, as slots does: once the wait is published, the work may resume this frame on
    // another thread, and a stop may unwind it, so nothing of it is written after.
    item_.handle = handle;

    if (context.stop_token.stop_possible()) {
      on_stop_.emplace(context.stop_token, stopper{this});
    }

    if (!state_->wait(item_, context.executor, context.owner, context.stop_token, stopped)) {
      on_stop_.reset();

      if (stopped) {
        async::detail::stop_chain(context.owner);
        return true;
      }

      return false;
    }

    return true;
  }

  async::detail::non_void_t<R> await_resume() {
    on_stop_.reset();
    return state_->take();
  }

private:
  struct stopper {
    call_wait *self;

    // The caller stops waiting; the work goes on. Touches nothing after stop_chain().
    void operator()() const noexcept {
      if (auto *const chain = self->state_->stop_waiting(self->item_)) {
        async::detail::stop_chain(chain);
      }
    }
  };

  std::shared_ptr<call_state<R>> state_;
  async::work_item item_{};
  std::optional<std::stop_callback<stopper>> on_stop_{};
};

static_assert(async::ContextAwaitable<call_wait<void>>);

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_CALL_STATE_HPP
