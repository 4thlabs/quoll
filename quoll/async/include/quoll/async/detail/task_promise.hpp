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

#ifndef QUOLL_ASYNC_DETAIL_TASK_PROMISE_HPP
#define QUOLL_ASYNC_DETAIL_TASK_PROMISE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/context_promise.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/detail/non_void.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace quoll::async {

template <class T>
class task;

} // namespace quoll::async

namespace quoll::async::detail {

template <class T>
class task_promise;

// Resumes the awaiter by symmetric transfer: no executor round trip, no stack growth. A task that ended on a stop
// resumes nothing: it hands the stop to the owner of its chain, which unwinds it, and touches nothing after.
struct task_final_awaiter {
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  template <class Promise>
  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
    auto &promise = handle.promise();

    if (promise.stopped()) {
      stop_chain(promise.owner());
      return std::noop_coroutine();
    }

    return promise.continuation();
  }

  void await_resume() const noexcept {}
};

// What task_promise<T> and task_promise<void> share: everything but return_value / return_void.
template <class T>
class task_promise_base : public context_promise {
public:
  [[nodiscard]] task<T> get_return_object() noexcept {
    return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(static_cast<task_promise<T> &>(*this))};
  }

  [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

  [[nodiscard]] task_final_awaiter final_suspend() noexcept {
    finished_ = true;
    return {};
  }

  // What a stop it was asked for throws, std::errc::operation_canceled, ends it on a stop: no catch above sees it, as
  // the code that still throws it, a program's or a library's, goes through a task.
  void unhandled_exception() noexcept {
    if (this->stop_requested() && is_a_stop(std::current_exception())) {
      stopped_ = true;
      return;
    }

    result_.template emplace<error_index>(std::current_exception());
  }

  [[nodiscard]] bool stopped() const noexcept { return stopped_; }

  void start(std::coroutine_handle<> continuation, await_context const &context) noexcept {
    continuation_ = continuation;
    set_context(context);
  }

  [[nodiscard]] std::coroutine_handle<> continuation() const noexcept { return continuation_; }

  T take() {
    if (result_.index() == error_index) {
      std::rethrow_exception(std::get<error_index>(result_));
    }

    if constexpr (!std::is_void_v<T>) {
      return std::move(std::get<value_index>(result_));
    }
  }

protected:
  static constexpr std::size_t value_index = 1;
  static constexpr std::size_t error_index = 2;

  std::variant<std::monostate, non_void_t<T>, std::exception_ptr> result_;

  // A frame is destroyed only once it will not resume again: before it starts, once it has ended, or suspended as its
  // chain unwinds after a stop, whatever destroys it (a task, or a handle held elsewhere). What its awaitables
  // registered points into it until then, and they undo it as they are destroyed.
  ~task_promise_base() {
    assert((!continuation_ || finished_ || this->unwinding()) &&
           "quoll::async::task: destroyed while it runs, with what it registered still pointing into its frame");
  }

  task_promise_base() = default;
  task_promise_base(task_promise_base const &) = delete;
  task_promise_base &operator=(task_promise_base const &) = delete;
  task_promise_base(task_promise_base &&) = delete;
  task_promise_base &operator=(task_promise_base &&) = delete;

private:
  std::coroutine_handle<> continuation_;
  bool finished_ = false;
  bool stopped_ = false;
};

template <class T>
class task_promise : public task_promise_base<T> {
  static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                "task<T>: T must not be over-aligned, as its coroutine frame is not");

public:
  template <class U = T>
    requires std::convertible_to<U &&, T>
  void return_value(U &&value) noexcept(std::is_nothrow_constructible_v<T, U &&>) {
    this->result_.template emplace<task_promise::value_index>(std::forward<U>(value));
  }
};

template <>
class task_promise<void> : public task_promise_base<void> {
public:
  void return_void() noexcept { result_.emplace<value_index>(); }
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_TASK_PROMISE_HPP
