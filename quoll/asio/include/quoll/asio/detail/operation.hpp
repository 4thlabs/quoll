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

#ifndef QUOLL_ASIO_DETAIL_OPERATION_HPP
#define QUOLL_ASIO_DETAIL_OPERATION_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/async/await_context.hpp>
#include <quoll/async/work_item.hpp>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::asio::detail {

// An asio asynchronous operation awaited by a coroutine. `initiation(handler)` starts it; the handler, run by the
// I/O object's executor, stores what the operation produced and posts the coroutine back to its own executor, which
// may be another one (a thread pool): await_resume hands the results over there.
//
// When the awaiter's stop token can stop, the operation is cancellable through `cancellation` (see there): it starts
// on the I/O object's executor, bound to the cancellation slot, and a stop request, from any thread, cancels it
// there. Otherwise it starts at once, on the awaiter's thread. A stop requested before the operation starts
// completes it with `operation_aborted` without starting it, as some operations (resolve) ignore cancellation slots.
//
// The handler points to the operation, in the frame of the suspended coroutine: that frame is destroyed only after it
// resumes, so the handler always finds it. A suspended chain of tasks is never destroyed from outside; destroying the
// I/O object's executor destroys the handler without running it.
//
// A failure to post the coroutine back, or to start the operation from the I/O object's executor, cannot reach the
// awaiter: it calls std::terminate (see async::Executor).
//
// Movable until it suspends, so that it can be co_await-ed as a temporary.
template <class Initiation, class... Values>
class operation {
public:
  using result_type = std::tuple<std::error_code, Values...>;

  operation(Initiation initiation,
            std::shared_ptr<cancellation> cancellation) noexcept(std::is_nothrow_move_constructible_v<Initiation>)
      : initiation_(std::move(initiation)), cancellation_(std::move(cancellation)) {}

  operation(operation const &) = delete;
  operation &operator=(operation const &) = delete;
  operation &operator=(operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  operation(operation &&other) noexcept(std::is_nothrow_move_constructible_v<Initiation>)
      : initiation_(std::move(other.initiation_)), cancellation_(std::move(other.cancellation_)) {}

  ~operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    item_.handle = handle;
    context_ = &context;

    if (context.stop_token.stop_requested()) {
      result_.emplace(std::error_code{::asio::error::operation_aborted}, Values{}...);
      context.executor.post(item_);
      return;
    }

    if (!context.stop_token.stop_possible()) {
      initiation_(completion{this, nullptr});
      return;
    }

    // Registered before the start: once started, the operation may complete and the coroutine resume and destroy
    // the callback on another thread.
    ticket_ = cancellation_->next();
    stop_callback_.emplace(context.stop_token, canceller{cancellation_, ticket_});
    ::asio::dispatch(cancellation_->executor(), starter{this});
  }

  [[nodiscard]] result_type await_resume() {
    stop_callback_.reset();
    assert(result_ && "asio operation resumed before its handler ran");
    return std::move(*result_);
  }

private:
  struct completion {
    operation *self;
    // The cancellation the operation was bound to, if any.
    cancellation *bound;

    void operator()(std::error_code error, Values... values) const noexcept {
      if (bound != nullptr) {
        bound->finish();
      }

      self->result_.emplace(error, std::move(values)...);
      self->context_->executor.post(self->item_);
    }
  };

  // Runs on the I/O object's executor.
  struct starter {
    operation *self;

    void operator()() const noexcept { self->start(); }
  };

  struct canceller {
    std::shared_ptr<cancellation> state;
    std::uint64_t ticket;

    void operator()() const noexcept { cancellation::request(state, ticket); }
  };

  void start() {
    // A request that arrived before the start found nothing to cancel: the token tells.
    if (context_->stop_token.stop_requested()) {
      completion{this, nullptr}(std::error_code{::asio::error::operation_aborted}, Values{}...);
      return;
    }

    initiation_(::asio::bind_cancellation_slot(cancellation_->start(ticket_), completion{this, cancellation_.get()}));
  }

  Initiation initiation_;
  std::shared_ptr<cancellation> cancellation_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  std::uint64_t ticket_ = 0;
  std::optional<result_type> result_;
  std::optional<std::stop_callback<canceller>> stop_callback_;
};

// Builds an operation, naming only the values its handler receives after the error code.
template <class... Values, class Initiation>
[[nodiscard]] operation<Initiation, Values...> make_operation(std::shared_ptr<cancellation> cancellation,
                                                              Initiation initiation) {
  return {std::move(initiation), std::move(cancellation)};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_OPERATION_HPP
