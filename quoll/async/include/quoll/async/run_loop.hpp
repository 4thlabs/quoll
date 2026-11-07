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

#ifndef QUOLL_ASYNC_RUN_LOOP_HPP
#define QUOLL_ASYNC_RUN_LOOP_HPP

#include <quoll/async/executor.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/work_item.hpp>

#include <cassert>
#include <cstddef>

namespace quoll::async {

/**
 * @brief Single-threaded execution context: a FIFO of coroutines resumed by run().
 *
 * The FIFO links the posted work items: posting never allocates and never throws.
 *
 * Needs no thread and never blocks, so it runs everywhere, WASM included; tests drive their coroutines with it.
 * Executors point to the loop, which therefore can be neither copied nor moved.
 *
 * Run the loop until it is empty before destroying it. A queued handle does not own its frame (a task posted by
 * yield() belongs to the task awaiting it), so the loop cannot destroy what is left: it would be leaked.
 *
 * Thread safety: none. post() and run() are called from one thread.
 */
class run_loop {
public:
  /** @brief Executor posting to a run_loop. Equal when they post to the same loop. */
  class executor {
  public:
    /** @brief Queues `item`, resumed by run(). */
    void post(work_item &item) const noexcept { loop_->push(item); }

    [[nodiscard]] bool operator==(executor const &) const noexcept = default;

  private:
    friend class run_loop;

    explicit executor(run_loop &loop) noexcept : loop_(&loop) {}

    run_loop *loop_;
  };

  using executor_type = executor;

  run_loop() = default;
  run_loop(run_loop const &) = delete;
  run_loop(run_loop &&) = delete;
  run_loop &operator=(run_loop const &) = delete;
  run_loop &operator=(run_loop &&) = delete;

  ~run_loop() { assert(head_ == nullptr && "run_loop destroyed with queued coroutines: run() it first"); }

  /** @brief An executor posting to this loop, which must outlive it. */
  [[nodiscard]] executor get_executor() noexcept QUOLL_LIFETIMEBOUND { return executor{*this}; }

  /**
   * @brief Resumes queued coroutines, including the ones they post, until the queue is empty.
   * @return The number of coroutines resumed.
   */
  std::size_t run() {
    std::size_t resumed = 0;

    while (head_ != nullptr) {
      auto *const item = head_;

      head_ = item->next;

      if (head_ == nullptr) {
        tail_ = nullptr;
      }

      // Read before resuming: the item may live in the frame the coroutine destroys.
      auto const handle = item->handle;

      handle.resume();
      ++resumed;
    }

    return resumed;
  }

private:
  void push(work_item &item) noexcept {
    item.next = nullptr;

    if (tail_ == nullptr) {
      head_ = &item;
    } else {
      tail_->next = &item;
    }

    tail_ = &item;
  }

  work_item *head_ = nullptr;
  work_item *tail_ = nullptr;
};

static_assert(Executor<run_loop::executor>);

} // namespace quoll::async

#endif // QUOLL_ASYNC_RUN_LOOP_HPP
