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

#ifndef QUOLL_ASYNC_DETAIL_TASK_GROUP_HPP
#define QUOLL_ASYNC_DETAIL_TASK_GROUP_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace quoll::async::detail {

// Tasks spawned by one owner, counted, so that the owner completes only once they have all ended: what they refer to
// then needs to outlive the owner only. Thread safety: started() and ended() may be called from any thread (the tasks
// may run on a thread pool).
class task_group {
public:
  // Waits until no task is left; resumes the awaiter on its executor.
  class [[nodiscard]] join_awaitable {
  public:
    explicit join_awaitable(task_group &group) noexcept : group_(&group) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, await_context const &context) {
      std::lock_guard const lock{group_->mutex_};

      if (group_->live_ == 0) {
        return false;
      }

      item_.handle = handle;
      group_->waiter_ = &item_;
      group_->executor_.emplace(context.executor);
      return true;
    }

    void await_resume() const noexcept {}

  private:
    task_group *group_;
    work_item item_{};
  };

  void started() {
    std::lock_guard const lock{mutex_};

    ++live_;
  }

  // The last connection resumes serve(), if it waits. Called from a completion handler: noexcept, as a post that fails
  // there would leave serve() suspended forever.
  void ended() noexcept {
    work_item *waiter = nullptr;
    std::optional<executor_ref> executor;

    {
      std::lock_guard const lock{mutex_};

      if (--live_ == 0 && waiter_ != nullptr) {
        waiter = std::exchange(waiter_, nullptr);
        executor = executor_;
      }
    }

    if (waiter != nullptr) {
      executor->post(*waiter);
    }
  }

  [[nodiscard]] join_awaitable join() noexcept { return join_awaitable{*this}; }

private:
  std::mutex mutex_;
  std::size_t live_ = 0;
  work_item *waiter_ = nullptr;
  // The executor of serve(), valid while it waits.
  std::optional<executor_ref> executor_;
};

static_assert(async::ContextAwaitable<task_group::join_awaitable>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_TASK_GROUP_HPP
