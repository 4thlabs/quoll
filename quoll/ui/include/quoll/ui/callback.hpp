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

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/call_state.hpp>
#include <quoll/ui/detail/ready.hpp>
#include <quoll/ui/detail/view_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

template <class Signature>
class callback;

namespace detail {

template <class T>
inline constexpr bool is_callback = false;

template <class Signature>
inline constexpr bool is_callback<callback<Signature>> = true;

// What refers to an object it does not hold, which a copy does not make the task's own.
template <class T>
inline constexpr bool is_view = false;

template <class CharT, class Traits>
inline constexpr bool is_view<std::basic_string_view<CharT, Traits>> = true;

template <class T, std::size_t Extent>
inline constexpr bool is_view<std::span<T, Extent>> = true;

// What may hold no function, which a callback made of it holds none either.
template <class T>
inline constexpr bool may_be_empty = std::is_pointer_v<T> || std::is_member_pointer_v<T>;

template <class Signature>
inline constexpr bool may_be_empty<std::function<Signature>> = true;

} // namespace detail

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
 * A caller already stopped starts nothing, and a work whose component is stopped before it starts does not run.
 *
 * Made outside a view, in a handler or a mounted() included, it runs the function as part of the caller's task: what
 * marks the component is its view running. The function returns `R`, or a task of `R`;
 * the arguments are copied into the task, so that none can be a reference to write to or a view, refused at compile
 * time; a pointer still points to what the caller gave, which must outlive the work. Copies of a callback share its
 * function. Every callback of one signature
 * that calls something compares equal, whatever its function captures: it calls back a component, which stays the
 * same, so that props holding one compare by the rest.
 *
 * Thread safety: none, as its app: called from a task on the app's executor, whose stop is asked for there, as those of
 * the app's own tasks are.
 */
template <class R, class... Args>
class callback<R(Args...)> {
  static_assert(((!std::is_lvalue_reference_v<Args> || std::is_const_v<std::remove_reference_t<Args>>) && ...),
                "quoll::ui: a callback's arguments are copied into its task: none can be a reference to write to");
  static_assert((!detail::is_view<std::remove_cvref_t<Args>> && ...),
                "quoll::ui: a callback's arguments are copied into its task: a view would still refer to the caller's "
                "object, which may go first");

  using function_type = std::function<async::task<R>(Args...)>;

public:
  /** @brief A callback calling nothing: a call of it throws `std::bad_function_call`. */
  callback() = default;

  /** @brief Calls `function` back, in the task of the component whose view makes it. Implicit, as for std::function. */
  template <class F>
    requires(!detail::is_callback<std::remove_cvref_t<F>> && std::invocable<std::decay_t<F> &, Args...> &&
             (std::same_as<std::invoke_result_t<std::decay_t<F> &, Args...>, async::task<R>> ||
              (std::is_void_v<R> && std::is_void_v<std::invoke_result_t<std::decay_t<F> &, Args...>>) ||
              (!std::is_void_v<R> && std::convertible_to<std::invoke_result_t<std::decay_t<F> &, Args...>, R>)) &&
             std::copy_constructible<std::decay_t<F>>)
  callback(F &&function) // NOLINT(google-explicit-constructor, hicpp-explicit-conversions)
      : function_(made_of(std::forward<F>(function))), target_(detail::current_target()) {}

  /** @brief Calls it back, with copies of `args`: see the class. */
  [[nodiscard]] async::task<R> operator()(Args... args) const {
    return call(function_, target_, std::decay_t<Args>(std::forward<Args>(args))...);
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
  [[nodiscard]] static async::task<R> call(std::shared_ptr<function_type const> function, detail::call_target target,
                                           std::decay_t<Args>... args) {
    if (!function) {
      throw std::bad_function_call{};
    }

    // Stopped already: it asks for nothing.
    auto const caller_stop = co_await async::current_stop_token();

    if (caller_stop.stop_requested()) {
      co_await async::detail::end_stopped{};
    }

    if (!target.app) {
      // Made outside an app, as for to_html: its component, if it had one, is gone with the page.
      if (target.of_component && target.owner.expired()) {
        co_await async::detail::end_stopped{};
      }

      if constexpr (std::is_void_v<R>) {
        co_await (*function)(std::move(args)...);
        co_return;
      } else {
        co_return co_await (*function)(std::move(args)...);
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

  [[nodiscard]] static async::task<> work(std::shared_ptr<function_type const> function,
                                          std::shared_ptr<detail::call_state<R>> state, std::decay_t<Args>... args) {
    detail::call_end<R> end{std::move(state)};

    // Its component stopped since the call was posted: it does not run, and the caller ends on the stop with it.
    auto const component_stop = co_await async::current_stop_token();

    if (component_stop.stop_requested()) {
      co_return;
    }

    try {
      if constexpr (std::is_void_v<R>) {
        co_await (*function)(std::move(args)...);
        end.give({});
      } else {
        end.give(co_await (*function)(std::move(args)...));
      }
    } catch (...) {
      if (!end.fail(std::current_exception())) {
        throw;
      }
    }
  }

  std::shared_ptr<function_type const> function_;
  detail::call_target target_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_CALLBACK_HPP
