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

#ifndef QUOLL_UI_DECORATORS_HPP
#define QUOLL_UI_DECORATORS_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/element.hpp>
#include <quoll/ui/state.hpp>

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief What follows an element after `|`: applying it completes the element. Make your own with decorator(). */
template <class F>
class basic_decorator {
public:
  explicit basic_decorator(F apply) noexcept(std::is_nothrow_move_constructible_v<F>) : apply_(std::move(apply)) {}

  [[nodiscard]] element operator()(element target) && {
    apply_(target);
    return target;
  }

private:
  F apply_;
};

/** @brief A decorator from what it does to an element: `decorator([](element &target) { ... })`. */
template <class F>
  requires std::invocable<F &, element &>
[[nodiscard]] basic_decorator<F> decorator(F apply) {
  return basic_decorator<F>{std::move(apply)};
}

/** @brief `text("hello") | tw("text-sm")`. */
template <class F>
[[nodiscard]] element operator|(element target, basic_decorator<F> applied) {
  return std::move(applied)(std::move(target));
}

/**
 * @brief Adds classes: Tailwind's, or anyone's.
 *
 * Tailwind finds the classes it generates by reading the sources, so write them whole: `tw("bg-red-500")`, never a name
 * put together at run time.
 */
[[nodiscard]] inline auto tw(std::string_view classes) {
  return decorator([classes = std::string{classes}](element &target) {
    if (classes.empty()) {
      return;
    }

    if (!target.classes.empty()) {
      target.classes += ' ';
    }

    target.classes += classes;
  });
}

/** @brief tw(), when `condition` holds. */
[[nodiscard]] inline auto tw_if(bool condition, std::string_view classes) {
  return tw(condition ? classes : std::string_view{});
}

/** @brief Names an element among its siblings, so that it keeps its node when they move. */
[[nodiscard]] inline auto key(std::string_view name) {
  return decorator([name = std::string{name}](element &target) { target.key = name; });
}

/** @brief key() from a number. */
template <std::integral T>
[[nodiscard]] auto key(T number) {
  return decorator([name = std::to_string(number)](element &target) { target.key = name; });
}

/** @brief Sets an attribute of the element. */
[[nodiscard]] inline auto attribute(std::string_view name, std::string_view value) {
  return decorator([name = std::string{name}, value = std::string{value}](element &target) {
    target.attributes.emplace_back(name, value);
  });
}

namespace detail {

[[nodiscard]] inline async::task<> no_work() { co_return; }

// What an event runs, from what the application gave: with the node's value or without, a task or a plain function. A
// task is returned for the caller to await while it holds a copy of this handler: a coroutine lambda keeps its captures
// in the lambda.
template <class F>
[[nodiscard]] event_handler to_handler(F function) {
  return [function = std::move(function)](std::string value) mutable -> async::task<> {
    if constexpr (std::invocable<F &, std::string>) {
      if constexpr (std::same_as<std::invoke_result_t<F &, std::string>, async::task<>>) {
        return function(std::move(value));
      } else {
        function(std::move(value));
        return no_work();
      }
    } else {
      if constexpr (std::same_as<std::invoke_result_t<F &>, async::task<>>) {
        return function();
      } else {
        function();
        return no_work();
      }
    }
  };
}

} // namespace detail

/**
 * @brief Runs `function` on the event `name` of the element: a plain function or one returning a `task<>`, given the
 * node's value or nothing.
 *
 * It stays alive until its task ends, even if the element is gone by then. What it captures by reference has to as
 * well: capture `this`, which lives as long as the app, whose stop() waits for the tasks.
 */
template <class F>
[[nodiscard]] auto on(std::string_view name, F function) {
  return decorator([name = std::string{name}, handler = detail::to_handler(std::move(function))](element &target) {
    target.events.push_back({name, handler});
  });
}

/** @brief on("click"). */
template <class F>
[[nodiscard]] auto on_click(F function) {
  return on("click", std::move(function));
}

/** @brief on("input"), given what the input holds now. */
template <class F>
[[nodiscard]] auto on_input(F function) {
  return on("input", std::move(function));
}

/** @brief Keeps an input and a state the same: the input shows the state, and typing changes it. */
[[nodiscard]] inline auto bind(state<std::string> &to) {
  return decorator([&to](element &target) {
    target.value = *to;
    target.events.push_back({"input", detail::to_handler([&to](std::string typed) { to.set(std::move(typed)); })});
  });
}

} // namespace quoll::ui

#endif // QUOLL_UI_DECORATORS_HPP
