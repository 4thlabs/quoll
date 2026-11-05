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

#ifndef QUOLL_ASYNC_DETAIL_SLOTS_HPP
#define QUOLL_ASYNC_DETAIL_SLOTS_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <system_error>

namespace quoll::async::detail {

// A number of places taken and given back, what waits for one waiting its turn, first come, first served. A stop
// request on what waits ends its wait with std::errc::operation_canceled. Thread safety: all of it from any thread, but
// release() posts what waits to its own executor from that thread, so that an executor whose post() is not thread-safe
// (a run_loop) keeps to its own thread. The slots outlive what waits, and a wait is not destroyed before it resumes,
// like any suspended chain of tasks.
class slots {
public:
  explicit slots(std::size_t capacity) : state_(queue{capacity, nullptr, nullptr}) {}

  class [[nodiscard]] acquire_awaitable {
  public:
    explicit acquire_awaitable(slots &owner) noexcept : owner_(&owner) {}

    acquire_awaitable(acquire_awaitable const &) = delete;
    acquire_awaitable &operator=(acquire_awaitable const &) = delete;
    acquire_awaitable &operator=(acquire_awaitable &&) = delete;

    // Moved into the awaiting coroutine before it suspends, when nothing has its address yet.
    acquire_awaitable(acquire_awaitable &&other) noexcept : owner_(other.owner_) {}

    ~acquire_awaitable() = default;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    // False, resuming the awaiter at once, with a place or stopped. The stop callback comes before the queue: once
    // queued, release() may resume the coroutine on another thread, and this frame may be gone.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, await_context const &context) {
      item_.handle = handle;
      executor_.emplace(context.executor);

      if (context.stop_token.stop_possible()) {
        stop_callback_.emplace(context.stop_token, canceller{this});
      }

      return owner_->state_.with([this, &context](queue &state) {
        if (context.stop_token.stop_requested()) {
          canceled_ = true;
          return false;
        }

        if (state.free > 0) {
          --state.free;
          return false;
        }

        state.append(this);
        return true;
      });
    }

    void await_resume() {
      // Waits for a callback running on another thread, which may be reading this.
      stop_callback_.reset();

      if (canceled_) {
        throw std::system_error{std::make_error_code(std::errc::operation_canceled)};
      }
    }

  private:
    friend class slots;

    struct canceller {
      acquire_awaitable *self;

      void operator()() const noexcept {
        auto const removed = self->owner_->state_.with([this](queue &state) {
          if (!state.remove(self)) {
            return false;
          }

          self->canceled_ = true;
          return true;
        });

        if (removed) {
          self->executor_->post(self->item_);
        }
      }
    };

    slots *owner_;
    work_item item_{};
    std::optional<executor_ref> executor_{};
    acquire_awaitable *previous_ = nullptr;
    acquire_awaitable *next_ = nullptr;
    bool queued_ = false;
    bool canceled_ = false;
    std::optional<std::stop_callback<canceller>> stop_callback_{};
  };

  /** @brief A place: at once if there is one, otherwise when release() gives it. */
  [[nodiscard]] acquire_awaitable acquire() noexcept { return acquire_awaitable{*this}; }

  /** @brief Gives a place back: to what waits first, if anything does. */
  void release() noexcept {
    auto *const woken = state_.with([](queue &state) -> acquire_awaitable * {
      auto *const first = state.first;

      if (first == nullptr) {
        ++state.free;
        return nullptr;
      }

      state.remove(first);
      return first;
    });

    if (woken != nullptr) {
      woken->executor_->post(woken->item_);
    }
  }

  /** @brief How many places are free now, which others may change as soon as this returns. */
  [[nodiscard]] std::size_t available() const {
    return state_.with([](queue const &state) { return state.free; });
  }

private:
  struct queue {
    std::size_t free;
    acquire_awaitable *first;
    acquire_awaitable *last;

    void append(acquire_awaitable *waiter) noexcept {
      waiter->queued_ = true;
      waiter->previous_ = last;
      waiter->next_ = nullptr;
      (last != nullptr ? last->next_ : first) = waiter;
      last = waiter;
    }

    // False if it is not waiting any more.
    bool remove(acquire_awaitable *waiter) noexcept {
      if (!waiter->queued_) {
        return false;
      }

      waiter->queued_ = false;
      (waiter->previous_ != nullptr ? waiter->previous_->next_ : first) = waiter->next_;
      (waiter->next_ != nullptr ? waiter->next_->previous_ : last) = waiter->previous_;
      return true;
    }
  };

  synchronized<queue> state_;
};

static_assert(ContextAwaitable<slots::acquire_awaitable>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_SLOTS_HPP
