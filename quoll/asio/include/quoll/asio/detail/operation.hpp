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

#include <quoll/async/await_context.hpp>
#include <quoll/async/work_item.hpp>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/error.hpp>

#include <cassert>
#include <coroutine>
#include <optional>
#include <stop_token>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::asio::detail {

// An asio asynchronous operation awaited by a coroutine. `initiation(handler)` starts it when the coroutine
// suspends; the handler, run by the I/O object's executor, stores what the operation produced and posts the
// coroutine back to its own executor, where await_resume hands the results over.
//
// A stop request on the await_context emits a terminal cancellation on `signal`, bound to the operation only when the
// token can stop. The signal is kept by the caller across operations, so that asio reuses the memory it gives the
// cancellation handler. The stop request comes from the thread running the I/O object's executor, which runs the
// operation's handler: an asio cancellation signal is not thread-safe.
//
// The handler points to the operation, in the frame of the suspended coroutine: that frame is destroyed only after it
// resumes, so the handler always finds it. A suspended chain of tasks is never destroyed from outside; destroying the
// I/O object's executor destroys the handler without running it.
//
// Movable until it suspends, so that it can be co_await-ed as a temporary.
template <class Initiation, class... Values>
class operation {
public:
  using result_type = std::tuple<std::error_code, Values...>;

  operation(Initiation initiation,
            ::asio::cancellation_signal &signal) noexcept(std::is_nothrow_move_constructible_v<Initiation>)
      : initiation_(std::move(initiation)), signal_(&signal) {}

  operation(operation const &) = delete;
  operation &operator=(operation const &) = delete;
  operation &operator=(operation &&) = delete;

  // Only moved before it suspends: nothing is pending yet.
  operation(operation &&other) noexcept(std::is_nothrow_move_constructible_v<Initiation>)
      : initiation_(std::move(other.initiation_)), signal_(other.signal_) {}

  ~operation() = default;

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle, async::await_context const &context) {
    item_.handle = handle;
    context_ = &context;

    // Already stopped: the operation is not started, as some (resolve) would not observe the cancellation.
    if (context.stop_token.stop_requested()) {
      result_.emplace(std::error_code{::asio::error::operation_aborted}, Values{}...);
      context.executor.post(item_);
      return;
    }

    if (!context.stop_token.stop_possible()) {
      initiation_(completion{this});
      return;
    }

    initiation_(::asio::bind_cancellation_slot(signal_->slot(), completion{this}));
    // After the start: asio never runs a handler inside the initiating function, and the callback, invoked at once
    // if stop was already requested, needs the operation to be started to cancel it.
    stop_callback_.emplace(context.stop_token, canceller{signal_});
  }

  [[nodiscard]] result_type await_resume() {
    stop_callback_.reset();
    assert(result_ && "asio operation resumed before its handler ran");
    return std::move(*result_);
  }

private:
  struct completion {
    operation *self;

    // noexcept: a post failing here would escape io_context::run() and leave the coroutine suspended forever, so it
    // terminates instead (see async::Executor).
    void operator()(std::error_code error, Values... values) const noexcept {
      self->result_.emplace(error, std::move(values)...);
      self->context_->executor.post(self->item_);
    }
  };

  struct canceller {
    ::asio::cancellation_signal *signal;

    void operator()() const noexcept { signal->emit(::asio::cancellation_type::terminal); }
  };

  Initiation initiation_;
  ::asio::cancellation_signal *signal_;
  async::await_context const *context_ = nullptr;
  async::work_item item_;
  std::optional<result_type> result_;
  std::optional<std::stop_callback<canceller>> stop_callback_;
};

// Builds an operation, naming only the values its handler receives after the error code.
template <class... Values, class Initiation>
[[nodiscard]] operation<Initiation, Values...> make_operation(::asio::cancellation_signal &signal,
                                                              Initiation initiation) {
  return {std::move(initiation), signal};
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_OPERATION_HPP
