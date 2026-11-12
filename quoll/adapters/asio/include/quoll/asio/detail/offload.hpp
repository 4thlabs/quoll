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

#ifndef QUOLL_ASIO_DETAIL_OFFLOAD_HPP
#define QUOLL_ASIO_DETAIL_OFFLOAD_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/execution/outstanding_work.hpp>
#include <asio/execution_context.hpp>
#include <asio/post.hpp>
#include <asio/prefer.hpp>
#include <asio/thread_pool.hpp>

#include <coroutine>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// The threads that blocking work goes to, one set per execution context, started by its first use: a program with no
// such work has no such threads. Two, as a read of a slow disk should not hold up the next one on a fast disk, and as
// this is no place for what wants to scale.
class offload_threads : public ::asio::execution_context::service {
public:
  // An id of its own, rather than its type: asio finds a service by it without RTTI too.
  static inline ::asio::execution_context::id id;

  explicit offload_threads(::asio::execution_context &context) : service(context) {}

  [[nodiscard]] ::asio::thread_pool &pool() noexcept { return pool_; }

private:
  // What is under way ends first: it refers to frames that outlive it.
  void shutdown() override { pool_.join(); }

  ::asio::thread_pool pool_{2};
};

// A call of `work` on those threads, awaited by a coroutine. A stop request seen before it starts, in line behind
// others too, cancels it. Counts as work until it has come back, so that a loop does not end under it. Everything it
// touches is in the frame of the suspended coroutine, which goes only after it resumes. Movable until it suspends.
class offload_operation {
public:
  offload_operation(::asio::any_io_executor executor, void (*work)(void *) noexcept, void *argument) noexcept
      : executor_(std::move(executor)), work_(work), argument_(argument) {}

  offload_operation(offload_operation const &) = delete;
  offload_operation &operator=(offload_operation const &) = delete;
  offload_operation &operator=(offload_operation &&) = delete;

  offload_operation(offload_operation &&other) noexcept
      : executor_(std::move(other.executor_)), work_(other.work_), argument_(other.argument_) {}

  ~offload_operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Returns false, resuming the awaiter at once, when stop is already requested.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    if (context.stop_token.stop_requested()) {
      canceled_ = true;
      return false;
    }

    item_.handle = handle;

    auto &threads = ::asio::use_service<offload_threads>(executor_.context());

    // A failure to post the coroutine back calls std::terminate (see async::Executor).
    ::asio::post(
        threads.pool(),
        [this, &context,
         keep_running = ::asio::prefer(executor_, ::asio::execution::outstanding_work.tracked)]() noexcept {
          if (context.stop_token.stop_requested()) {
            canceled_ = true;
          } else {
            work_(argument_);
          }

          context.executor.post(item_);
        }
    );
    return true;
  }

  void await_resume() const {
    if (canceled_) {
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "quoll::asio::executor::offload"};
    }
  }

private:
  ::asio::any_io_executor executor_;
  void (*work_)(void *) noexcept;
  void *argument_;
  async::work_item item_;
  bool canceled_ = false;
};

static_assert(async::ContextAwaitable<offload_operation>);

// See async::Offloader.
[[nodiscard]] inline async::task<>
offload(::asio::any_io_executor executor, void (*work)(void *) noexcept, void *argument) {
  co_await offload_operation{std::move(executor), work, argument};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_OFFLOAD_HPP
