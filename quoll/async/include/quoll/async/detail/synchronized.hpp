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

#ifndef QUOLL_ASYNC_DETAIL_SYNCHRONIZED_HPP
#define QUOLL_ASYNC_DETAIL_SYNCHRONIZED_HPP

#include <quoll/async/detail/something_to_await.hpp>

#include <concepts>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace quoll::async::detail {

/**
 * A value and the mutex that protects it, in one: the value is only given to a function, under the lock, so that no
 * access can forget it.
 *
 * The function may not return a reference, which would outlive the lock, nor something to await: a coroutine runs past
 * with(), after the lock is released, and would read the value without it. These are refused at compile time, but a
 * pointer or a view it returns, or an awaitable inside what it returns, cannot be seen: not a guarantee, as what is
 * reached past the lock is the caller's. Clang's -Wthread-safety would need a mutex of our own, annotated, as
 * libstdc++'s is not, and only Clang would check it.
 *
 * Thread safety: with() may be called from any thread, but not from the function it was given, for the same object: the
 * mutex is not recursive.
 */
template <class T>
class synchronized {
public:
  template <class... Arguments>
    requires std::constructible_from<T, Arguments...>
  explicit synchronized(Arguments &&...arguments) : value_(std::forward<Arguments>(arguments)...) {}

  // Not constrained: checking the other overload would instantiate a generic function with the wrong constness.
  /** @brief What `function` returns, given the value, under the lock. */
  template <class F>
  auto with(F &&function) {
    return call_locked<std::invoke_result_t<F, T &>>(mutex_, value_, std::forward<F>(function));
  }

  /** @copydoc with */
  template <class F>
  auto with(F &&function) const {
    return call_locked<std::invoke_result_t<F, T const &>>(mutex_, value_, std::forward<F>(function));
  }

private:
  template <class Result, class Value, class F>
  static Result call_locked(std::mutex &mutex, Value &value, F &&function) {
    static_assert(!std::is_reference_v<Result>, "quoll: a reference would outlive the lock of synchronized");
    static_assert(!SomethingToAwait<Result>, "quoll: what synchronized gives runs past its lock if it is awaited");

    std::scoped_lock const lock{mutex};

    return std::invoke(std::forward<F>(function), value);
  }

  mutable std::mutex mutex_;
  T value_;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_SYNCHRONIZED_HPP
