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

#ifndef QUOLL_UI_ACTION_HPP
#define QUOLL_UI_ACTION_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/run_slot.hpp>
#include <quoll/ui/detail/target.hpp>
#include <quoll/ui/run_error.hpp>
#include <quoll/ui/state.hpp>

#include <cassert>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief What a call of an action does while another is under way. */
enum class overlap {
  /** @brief It is refused: it runs nothing, its result run_refused, as a second click on a button that waits. */
  refuse,
  /** @brief It stops the one under way, whose answer then writes nothing, and runs. */
  restart,
};

namespace detail {

// What an action's arguments and result may be: kept, as values.
template <class R, class... Args>
inline constexpr bool action_signature = kept_as_values<R> && kept_as_values<std::tuple<Args...>>;

} // namespace detail

template <class Signature, overlap Policy = overlap::refuse>
class action;

/**
 * @brief A function a handler of a component runs, once at a time by default, what it gives written into a target
 * state, as an effect's: whether it is under way, and why it failed, for a button to wait on and a view to show.
 *
 * @code
 * struct note_form {
 *   state<std::string> draft;
 *   // Whether it is being added, and why it was not.
 *   state<std::tuple<bool, std::exception_ptr>> adding;
 *
 *   action<notes::note(std::string)> add{notes_client::create, adding};
 *
 *   task<> send() {
 *     if (auto const added = co_await add.run(*draft)) {
 *       draft.set({});
 *     }
 *   }
 *
 *   element view() {
 *     auto const &[busy, error] = *adding;
 *     auto const send_it = [this] { return send(); };
 *
 *     return div({
 *         input() | bind(draft),
 *         busy ? button("Adding…") | attribute("disabled", "") : button("Add") | on_click(send_it),
 *     });
 *   }
 * };
 * @endcode
 *
 * A member of a component, after its target, a state of the same component. run() runs the function in the task
 * awaiting it, a handler's or mounted()'s, and gives what it gave or what it threw, before giving its task too, as a
 * std::expected, not thrown again. The target receives what an effect's does, by the type of its elements: whether a
 * run is under way, why the last failed, and the values it gave, copied, as its caller is given them too. A call while
 * one is under way is refused, run_refused at once, the target left as it is, or, with `overlap::restart`, stops it, so
 * that it writes nothing, even one that ignored the stop and came anyway, and gives run_replaced to its caller, which
 * goes on. A stop of the caller, as a timeout, ends the run and its caller, as a stop does, the target loading no more.
 * With no target, only its caller hears of it.
 *
 * Awaited from a task of its component, which the component outlives: a component that goes stops its runs. Neither
 * copied nor moved: a run under way refers to it. Its arguments are copied into the run, given to the function as
 * rvalues: none can be a reference or a view, which would outlive what it refers to. Thread safety: none, as a state.
 */
template <class R, class... Args, overlap Policy>
class action<R(Args...), Policy> {
  static_assert(detail::action_signature<R, Args...>,
                "quoll::ui::action: its arguments are copied into the run, and its result kept: none can be a "
                "reference or a view");

public:
  using result_type = std::expected<R, std::exception_ptr>;
  // Given the run's own copies, as rvalues: one taking them by reference refers to the run's copy.
  using function_type = std::function<async::task<R>(Args &&...)>;

  /**
   * @brief Runs `function`, which takes the arguments and returns a task of the result, its caller alone hearing of it.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class F>
    requires std::constructible_from<function_type, F>
  explicit action(F function) : function_(std::move(function)) {
    detail::check_function(function_, "quoll::ui::action: no function");
  }

  /**
   * @brief Runs `function`, which takes the arguments and returns a task of the result, written into `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class F, class T>
    requires std::constructible_from<function_type, F>
  action(F function, state<T> &target) : function_(std::move(function)), target_(target, std::true_type{}) {
    detail::check_target<T, R, true>();
    detail::check_function(function_, "quoll::ui::action: no function");
  }

  action(action const &) = delete;
  action(action &&) = delete;
  action &operator=(action const &) = delete;
  action &operator=(action &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~action() { assert(slot_.frames == 0 && "quoll::ui::action: destroyed while a run is under way"); }

  /**
   * @brief Runs it, as its overlap says: what it gave, or what it threw, as its target then holds; run_refused or
   * run_replaced for a call that gave no result of its own.
   */
  [[nodiscard]] async::task<result_type> run(Args... args) QUOLL_LIFETIMEBOUND {
    // Called once the run before is stopped, or not at all once refused: what it throws is this run's failure.
    auto make = [this, ... args = std::move(args)]() mutable { return function_(std::move(args)...); };

    auto ask = [this]() -> std::optional<std::stop_token> {
      if (Policy == overlap::refuse && slot_.pending) {
        return std::nullopt;
      }

      return detail::ask(slot_);
    };

    return detail::run_in<R>(slot_, target_, std::move(make), std::move(ask));
  }

private:
  function_type function_;
  detail::target_ref<R> target_;
  detail::run_slot slot_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_ACTION_HPP
