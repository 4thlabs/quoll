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

#ifndef QUOLL_UI_COMPONENT_CALLBACK_HPP
#define QUOLL_UI_COMPONENT_CALLBACK_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/component/call.hpp>
#include <quoll/ui/detail/component/ready.hpp>
#include <quoll/ui/detail/view/view_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace quoll::ui {

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
 *     auto const remove = [&given] {
 *       return given.on_remove(given.item.id);
 *     };
 *
 *     return button("Delete") | on_click(remove);
 *   }
 * };
 *
 * // In the page's view:
 * auto const on_remove = [this](std::int64_t id) {
 *   return remove(id);
 * };
 *
 * return component<note_row>({.item = item, .on_remove = on_remove});
 * @endcode
 *
 * Made from a function in a view, it holds the component whose view that is, or the app for its own view. Calling it
 * gives a task: awaited, it starts the function as a task of that component, under its stop, and waits for its end,
 * giving what it gives and throwing what it throws. A caller that stops, as the component calling it goes, stops
 * waiting, and the work goes on, as it belongs to the component that wrote it; what it throws then goes to the app's
 * on_error. A work that ends on a stop, as its own component goes, ends the caller on the stop too. Once the app has
 * stopped, or is gone, or the component is gone, a call ends on a stop and runs nothing: a callback holds its
 * component weakly, so that one kept by its own component leaves no cycle. A call once nothing holds the props of the
 * view that made it any more, that view replaced, throws `std::logic_error`, as what the function refers to of it, its
 * `given`, may be gone: a callback is good for the view that made it, which each view makes anew; while something
 * holds those props, a handler's task of that view or of a component below, the call runs, with that `given`.
 *
 * A caller already stopped starts nothing, and a work whose component is stopped before it starts does not run.
 *
 * Made outside a view, in a handler or a mounted() included, it knows no component: it runs the function as part of
 * the caller's task, as a std::function would, and what the function captures is the caller's to keep alive. What marks
 * the component is its view running. The function returns `R`, or a task of `R`;
 * the arguments are copied into the task, so that none can be a reference to write to or a view, refused at compile
 * time, as is a result that would refer to what the work held; a view inside an optional, a pair, a tuple, a
 * variant or an expected is refused too; a pointer, or a view in a container or inside a type of the program's, as a
 * member of a struct, still points to what the caller gave, which must outlive the work. Copies of a
 * callback share its function. Every callback of one signature that calls something compares equal, whatever its
 * function captures: it calls back a component, which stays the same, so that props holding one compare by the rest.
 *
 * Thread safety: none, as its app: called from a task on the app's executor, whose stop is asked for there, as those of
 * the app's own tasks are.
 */
template <class R, class... Args>
class callback<R(Args...)> {
  static_assert(((!std::is_lvalue_reference_v<Args> || std::is_const_v<std::remove_reference_t<Args>>) && ...),
                "quoll::ui: a callback's arguments are copied into its task: none can be a reference to write to");
  static_assert((!detail::holds_view<std::remove_cvref_t<Args>> && ...),
                "quoll::ui: a callback's arguments are copied into its task: a view would still refer to the caller's "
                "object, which may go first");
  static_assert(!detail::holds_view<R>,
                "quoll::ui: a callback's result reaches its caller once its work has ended: it cannot refer to what "
                "the work held");

  // Given the task's own copies, as rvalues: a function taking one by reference refers to the task's copy, which lives
  // as long as the task, and one taking it by value moves it.
  using function_type = std::function<async::task<R>(std::decay_t<Args> &&...)>;

public:
  /** @brief A callback calling nothing: a call of it throws `std::bad_function_call`. */
  callback() = default;

  /** @brief Calls `function` back, in the task of the component whose view makes it. Implicit, as for std::function. */
  template <class F>
    requires(!detail::is_callback<std::remove_cvref_t<F>> &&
             std::invocable<std::decay_t<F> &, std::decay_t<Args> && ...> &&
             (std::same_as<std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args> && ...>, async::task<R>> ||
              (std::is_void_v<R> &&
               std::is_void_v<std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args> && ...>>) ||
              (!std::is_void_v<R> &&
               std::convertible_to<std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args> && ...>, R>)) &&
             std::copy_constructible<std::decay_t<F>>)
  callback(F &&function) // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
      : function_(made_of(std::forward<F>(function))), target_(detail::current_target()) {}

  /** @brief Calls it back, with copies of `args`: see the class. */
  [[nodiscard]] async::task<R> operator()(Args... args) const {
    return detail::call_in<R>(function_, target_, std::decay_t<Args>(std::forward<Args>(args))...);
  }

  /** @brief Whether it calls something. */
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(function_); }

  /** @brief Whether both call something, or neither: see the class. */
  [[nodiscard]] friend bool operator==(callback const &left, callback const &right) noexcept {
    return static_cast<bool>(left.function_) == static_cast<bool>(right.function_);
  }

private:
  // None for a function that is none, a null pointer or an empty std::function, as std::function does.
  template <class F>
  [[nodiscard]] static std::shared_ptr<function_type const> made_of(F &&function) {
    if constexpr (detail::may_be_empty<std::decay_t<F>>) {
      if (!function) {
        return nullptr;
      }
    }

    return std::make_shared<function_type const>(adapt(std::forward<F>(function)));
  }

  template <class F>
  [[nodiscard]] static function_type adapt(F &&function) {
    return [held = std::decay_t<F>(std::forward<F>(function))](std::decay_t<Args> &&...args) mutable -> async::task<R> {
      using result = std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args> &&...>;

      if constexpr (std::same_as<result, async::task<R>>) {
        return std::invoke(held, std::move(args)...);
      } else if constexpr (std::is_void_v<R>) {
        std::invoke(held, std::move(args)...);
        return detail::done();
      } else {
        return detail::ready<R>(std::invoke(held, std::move(args)...));
      }
    };
  }

  std::shared_ptr<function_type const> function_;
  detail::call_target target_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_CALLBACK_HPP
