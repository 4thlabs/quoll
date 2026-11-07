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
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <cstddef>
#include <optional>
#include <utility>

namespace quoll::async::detail {

// Tasks spawned by one owner, counted, so that the owner completes only once they have all ended: what they refer to
// then needs to outlive the owner only. One owner awaits join(), once its last task has started: a second one would
// take its place. Thread safety: started(), ended() and live() may be called from any thread (the tasks may run on a
// thread pool).
class task_group {
public:
  // Waits until no task is left; resumes the awaiter on its executor.
  class [[nodiscard]] join_awaitable {
  public:
    explicit join_awaitable(task_group &group) noexcept : group_(&group) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, await_context const &context) {
      return group_->state_.with([&](counted &state) {
        if (state.live == 0) {
          return false;
        }

        item_.handle = handle;
        state.waiter = &item_;
        state.executor.emplace(context.executor);
        return true;
      });
    }

    void await_resume() const noexcept {}

  private:
    task_group *group_;
    work_item item_{};
  };

  void started() {
    state_.with([](counted &state) { ++state.live; });
  }

  // The last connection resumes serve(), if it waits. Called from a completion handler: noexcept, as a post failing
  // there would leave serve() suspended forever.
  void ended() noexcept {
    auto const woken = state_.with([](counted &state) -> std::optional<std::pair<work_item *, executor_ref>> {
      if (--state.live != 0 || state.waiter == nullptr) {
        return std::nullopt;
      }

      return std::pair{std::exchange(state.waiter, nullptr), *state.executor};
    });

    if (woken) {
      woken->second.post(*woken->first);
    }
  }

  [[nodiscard]] join_awaitable join() noexcept QUOLL_LIFETIMEBOUND { return join_awaitable{*this}; }

  // How many tasks run now, which others may change as soon as this returns.
  [[nodiscard]] std::size_t live() const {
    return state_.with([](counted const &state) { return state.live; });
  }

private:
  struct counted {
    std::size_t live = 0;
    work_item *waiter = nullptr;
    // The executor of what waits, valid meanwhile.
    std::optional<executor_ref> executor{};
  };

  synchronized<counted> state_;
};

static_assert(async::ContextAwaitable<task_group::join_awaitable>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_TASK_GROUP_HPP
