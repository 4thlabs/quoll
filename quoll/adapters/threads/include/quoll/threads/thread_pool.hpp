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

#ifndef QUOLL_THREADS_THREAD_POOL_HPP
#define QUOLL_THREADS_THREAD_POOL_HPP

#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/work_item.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <vector>

namespace quoll::threads {

/**
 * @brief Execution context resuming coroutines on a fixed set of worker
 * threads.
 *
 * post() never allocates, blocks or throws: it pushes the work item onto a
 * lock-free stack and wakes a worker. The workers take the items in the order
 * they were posted, and resume them concurrently.
 *
 * Destruction drains the pool: the destructor lets the workers resume what is
 * queued, including what those coroutines post, then joins them. Like run_loop,
 * the pool cannot destroy a queued coroutine, whose frame it does not own.
 * Every post must therefore happen before destruction starts: a coroutine woken
 * from outside the pool (a timer, a socket of another context) must have been
 * resumed by then.
 *
 * A worker running a coroutine that lets an exception escape resume() calls
 * std::terminate; task and co_spawn catch theirs.
 *
 * Thread safety: post() may be called from any thread, concurrently. Construct
 * and destroy the pool from a thread that is not one of its workers.
 */
class thread_pool {
public:
  /** @brief Executor posting to a thread_pool. Equal when they post to the same
   * pool. */
  class executor {
  public:
    void post(async::work_item &item) const noexcept { pool_->push(item); }

    [[nodiscard]] bool operator==(executor const &) const noexcept = default;

  private:
    friend class thread_pool;

    explicit executor(thread_pool &pool) noexcept : pool_(&pool) {}

    thread_pool *pool_;
  };

  using executor_type = executor;

  /** @brief Starts one worker per hardware thread, or a single one when that
   * number is unknown. */
  thread_pool();

  /**
   * @brief Starts `threads` workers.
   * @throws std::invalid_argument if `threads` is 0; std::system_error if a
   * thread cannot be started.
   */
  explicit thread_pool(std::size_t threads);

  thread_pool(thread_pool const &) = delete;
  thread_pool(thread_pool &&) = delete;
  thread_pool &operator=(thread_pool const &) = delete;
  thread_pool &operator=(thread_pool &&) = delete;

  ~thread_pool();

  [[nodiscard]] executor get_executor() noexcept QUOLL_LIFETIMEBOUND { return executor{*this}; }

private:
  void push(async::work_item &item) noexcept;
  [[nodiscard]] async::work_item *pop();
  void work(std::stop_token const &token);
  void stop() noexcept;

  std::atomic<async::work_item *> incoming_{nullptr};
  std::atomic<std::uint32_t> epoch_{0};
  // Taken from incoming_ under its lock, in posting order.
  async::detail::synchronized<async::work_item *> ready_{nullptr};
  // Last: joined before the queue they read is destroyed.
  std::vector<std::jthread> workers_;
};

static_assert(async::Executor<thread_pool::executor>);

} // namespace quoll::threads

#endif // QUOLL_THREADS_THREAD_POOL_HPP
