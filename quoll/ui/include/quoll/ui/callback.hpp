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

#ifndef QUOLL_UI_CALLBACK_HPP
#define QUOLL_UI_CALLBACK_HPP

#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/call_state.hpp>
#include <quoll/ui/detail/view_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace quoll::ui {

template <class Signature>
class callback;

/**
 * @brief What a component gives a component of its view to call it back: a function of its own, which runs as one of
 * its tasks, not the caller's.
 *
 * @code
 * struct note_row {
 *   struct props {
 *     notes::note item;
 *     callback<void(std::int64_t)> on_remove;
 *   };
 *
 *   element view(props const &given) {
 *     return button("Delete") | on_click([&given] { return given.on_remove(given.item.id); });
 *   }
 * };
 *
 * // In the page's view:
 * component<note_row>({.item = item, .on_remove = [this](std::int64_t id) { return remove(id); }})
 * @endcode
 *
 * Made from a function in a view, it holds the component whose view that is, or the app for its own view. Calling it
 * gives a task: awaited, it starts the function as a task of that component, under its stop, and waits for its end,
 * giving what it gives and throwing what it throws. A caller that stops, as the component calling it goes, stops
 * waiting, and the work goes on, as it belongs to the component that wrote it; what it throws then goes to the app's
 * on_error. A work that ends on a stop, as its own component goes, ends the caller on the stop too. Once the app has
 * stopped, or is gone, or the component is gone, a call ends on a stop and runs nothing: a callback holds its
 * component weakly, so that one kept by its own component leaves no cycle.
 *
 * Made outside a view, it runs the function as part of the caller's task. The function returns `R`, or a task of `R`;
 * the arguments are copied into the task. Every callback of one signature compares equal: it calls back a component,
 * which stays the same, so that props holding one compare by the rest.
 *
 * Thread safety: none, as its app.
 */
template <class R, class... Args>
class callback<R(Args...)> {
  using function_type = std::function<async::task<R>(Args...)>;

public:
  /** @brief A callback calling nothing: calling it throws `std::bad_function_call`. */
  callback() = default;

  /** @brief Calls `function` back, in the task of the component whose view makes it. Implicit, as for std::function. */
  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, callback> && std::invocable<std::decay_t<F> &, Args...> &&
             (std::same_as<std::invoke_result_t<std::decay_t<F> &, Args...>, async::task<R>> ||
              (std::is_void_v<R> && std::is_void_v<std::invoke_result_t<std::decay_t<F> &, Args...>>) ||
              (!std::is_void_v<R> && std::convertible_to<std::invoke_result_t<std::decay_t<F> &, Args...>, R>)) &&
             std::copy_constructible<std::decay_t<F>>)
  callback(F &&function) // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
      : function_(adapt(std::forward<F>(function))), target_(detail::current_target()) {}

  /** @brief Calls it back, with copies of `args`: see the class. */
  [[nodiscard]] async::task<R> operator()(Args... args) const {
    return call(function_, target_, std::decay_t<Args>(std::forward<Args>(args))...);
  }

  /** @brief Whether it calls something. */
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(function_); }

  /** @brief Always: see the class. */
  [[nodiscard]] friend bool operator==(callback const &, callback const &) noexcept { return true; }

private:
  template <class F>
  [[nodiscard]] static function_type adapt(F &&function) {
    return [held = std::decay_t<F>(std::forward<F>(function))](Args... args) mutable -> async::task<R> {
      using result = std::invoke_result_t<std::decay_t<F> &, Args...>;

      if constexpr (std::same_as<result, async::task<R>>) {
        return std::invoke(held, std::forward<Args>(args)...);
      } else if constexpr (std::is_void_v<R>) {
        std::invoke(held, std::forward<Args>(args)...);
        return detail::done();
      } else {
        return detail::ready<R>(std::invoke(held, std::forward<Args>(args)...));
      }
    };
  }

  // Owns what it needs: the callback may go before the task ends.
  [[nodiscard]] static async::task<R> call(function_type function, detail::call_target target,
                                           std::decay_t<Args>... args) {
    if (!target.app) {
      if constexpr (std::is_void_v<R>) {
        co_await function(std::move(args)...);
        co_return;
      } else {
        co_return co_await function(std::move(args)...);
      }
    }

    auto state = std::make_shared<detail::call_state<R>>();

    if (!detail::spawn_in(target, work(std::move(function), state, std::move(args)...))) {
      co_await async::detail::end_stopped{};
    }

    if constexpr (std::is_void_v<R>) {
      co_await detail::call_wait<R>{std::move(state)};
    } else {
      co_return co_await detail::call_wait<R>{std::move(state)};
    }
  }

  [[nodiscard]] static async::task<> work(function_type function, std::shared_ptr<detail::call_state<R>> state,
                                          std::decay_t<Args>... args) {
    detail::call_end<R> end{std::move(state)};

    try {
      if constexpr (std::is_void_v<R>) {
        co_await function(std::move(args)...);
        end.give({});
      } else {
        end.give(co_await function(std::move(args)...));
      }
    } catch (...) {
      if (!end.fail(std::current_exception())) {
        throw;
      }
    }
  }

  function_type function_;
  detail::call_target target_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_CALLBACK_HPP
