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

#ifndef QUOLL_TEST_VIRTUAL_TIME_HPP
#define QUOLL_TEST_VIRTUAL_TIME_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <optional>
#include <stop_token>
#include <vector>

namespace quoll::test {

/**
 * @brief A run_loop whose executor offers a timer on a virtual clock, for tests that neither wait nor depend on the
 * machine's speed. Hand it to quoll::test::run().
 *
 * run() drains the loop, then jumps to the earliest deadline, until nothing is left to run or to wait for. Tasks left
 * waiting for anything else are not completed, as with a plain run_loop.
 *
 * Thread safety: none, as run_loop.
 */
class virtual_time {
public:
  class executor {
  public:
    using services = async::service_list<async::timer_ref>;

    void post(async::work_item &item) const noexcept { time_->loop_.get_executor().post(item); }

    [[nodiscard]] async::task<bool> wait_for(std::chrono::nanoseconds duration) const {
      co_return co_await sleeper{*time_, duration};
    }

    [[nodiscard]] std::chrono::nanoseconds now() const noexcept { return time_->now(); }

    [[nodiscard]] async::timer_ref query(async::service_tag<async::timer_ref>) const { return async::timer_ref{*this}; }

    [[nodiscard]] bool operator==(executor const &) const noexcept = default;

  private:
    friend class virtual_time;

    explicit executor(virtual_time &time) noexcept : time_(&time) {}

    virtual_time *time_;
  };

  virtual_time() = default;
  virtual_time(virtual_time const &) = delete;
  virtual_time(virtual_time &&) = delete;
  virtual_time &operator=(virtual_time const &) = delete;
  virtual_time &operator=(virtual_time &&) = delete;
  ~virtual_time() = default;

  [[nodiscard]] executor get_executor() noexcept { return executor{*this}; }

  /** @brief The time elapsed on the virtual clock. */
  [[nodiscard]] std::chrono::nanoseconds now() const noexcept { return now_; }

  void run() {
    loop_.run();

    while (!sleepers_.empty()) {
      auto *const next = *std::ranges::min_element(sleepers_, {}, [](sleeper const *one) { return one->deadline(); });

      now_ = std::max(now_, next->deadline());
      next->finish(true);
      loop_.run();
    }
  }

private:
  class sleeper {
  public:
    sleeper(virtual_time &time, std::chrono::nanoseconds duration) noexcept
        : time_(&time),
          deadline_(duration > std::chrono::nanoseconds::max() - time.now_ ? std::chrono::nanoseconds::max()
                                                                           : time.now_ + duration) {}

    // Only moved before it suspends: nothing is pending yet.
    sleeper(sleeper &&other) noexcept : time_(other.time_), deadline_(other.deadline_) {}

    sleeper(sleeper const &) = delete;
    sleeper &operator=(sleeper const &) = delete;
    sleeper &operator=(sleeper &&) = delete;
    ~sleeper() = default;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
      if (context.stop_token.stop_requested()) {
        return false;
      }

      item_.handle = handle;
      context_ = &context;
      time_->sleepers_.push_back(this);
      stop_callback_.emplace(context.stop_token, canceller{this});
      return true;
    }

    [[nodiscard]] bool await_resume() noexcept {
      stop_callback_.reset();
      return elapsed_;
    }

    [[nodiscard]] std::chrono::nanoseconds deadline() const noexcept { return deadline_; }

    void finish(bool elapsed) {
      std::erase(time_->sleepers_, this);
      elapsed_ = elapsed;
      finished_ = true;
      context_->executor.post(item_);
    }

  private:
    struct canceller {
      sleeper *self;

      void operator()() const {
        if (!self->finished_) {
          self->finish(false);
        }
      }
    };

    virtual_time *time_;
    std::chrono::nanoseconds deadline_;
    async::await_context const *context_ = nullptr;
    async::work_item item_;
    bool elapsed_ = false;
    bool finished_ = false;
    std::optional<std::stop_callback<canceller>> stop_callback_;
  };

  static_assert(async::ContextAwaitable<sleeper>);

  async::run_loop loop_;
  std::chrono::nanoseconds now_{0};
  std::vector<sleeper *> sleepers_;
};

static_assert(async::Timer<virtual_time::executor>);

} // namespace quoll::test

#endif // QUOLL_TEST_VIRTUAL_TIME_HPP
