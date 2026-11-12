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

template <class T>
class shared;

/** @brief What a call of an action does while another is under way. */
enum class overlap {
  /** @brief It is refused: it runs nothing, its result run_refused, as a second click on a button that waits. */
  refuse,
  /** @brief It stops the one under way, whose answer then writes nothing, and runs. */
  restart,
};

namespace detail {

// What an action's function gives, the arguments it takes, and whose member it is: a task-returning free function or
// member function, or none, which the action refuses with a message of its own.
template <class F>
struct action_function {
  static constexpr bool valid = false;
  using result = void;
  using arguments = std::tuple<>;
  using owner = void;
  static constexpr bool writes_arguments = false;
};

// What a free function and a member function share: the parameters as values, and whether one is written back.
template <class R, class Owner, class... Args>
struct function_parts {
  using result = R;
  using arguments = std::tuple<std::decay_t<Args>...>;
  using owner = Owner;
  static constexpr bool valid = true;
  static constexpr bool writes_arguments =
      (... || (std::is_lvalue_reference_v<Args> && !std::is_const_v<std::remove_reference_t<Args>>));
};

template <class R, class... Args>
struct action_function<async::task<R> (*)(Args...)> : function_parts<R, void, Args...> {};

template <class R, class... Args>
struct action_function<async::task<R> (*)(Args...) noexcept> : action_function<async::task<R> (*)(Args...)> {};

template <class R, class C, class... Args>
struct action_function<async::task<R> (C::*)(Args...)> : function_parts<R, C, Args...> {};

template <class R, class C, class... Args>
struct action_function<async::task<R> (C::*)(Args...) const> : action_function<async::task<R> (C::*)(Args...)> {};

template <class R, class C, class... Args>
struct action_function<async::task<R> (C::*)(Args...) noexcept> : action_function<async::task<R> (C::*)(Args...)> {};

template <class R, class C, class... Args>
struct action_function<async::task<R> (C::*)(Args...) const noexcept>
    : action_function<async::task<R> (C::*)(Args...)> {};

// What is told of a run that gave R: the value, or nothing for void.
template <class R>
struct success_of {
  using type = std::function<void(R const &)>;
};

template <>
struct success_of<void> {
  using type = std::function<void()>;
};

// run() and with() of an action, taking the function's parameters as they are, so that a braced argument, as
// `{.name = "tea"}`, builds the parameter it is given for.
template <class Action, class Arguments>
class action_calls;

template <class Action, class... Ts>
class action_calls<Action, std::tuple<Ts...>> {
public:
  /**
   * @brief Runs it, as its overlap says: what it gave, or what it threw, as its target then holds; run_refused or
   * run_replaced for a call that gave no result of its own.
   */
  [[nodiscard]] auto run(Ts... args) QUOLL_LIFETIMEBOUND {
    return self().run_with(std::tuple<Ts...>{std::move(args)...});
  }

  /**
   * @brief A handler running it with copies of `args`, for on_click: what it gave goes to its target and to what
   * follows a success, and a failure its target has no place for to the app's on_error.
   */
  [[nodiscard]] auto with(Ts... args) QUOLL_LIFETIMEBOUND {
    static_assert((std::copy_constructible<Ts> && ...),
                  "quoll::ui::action: with() copies its arguments for each click: they are copyable");

    // Not a coroutine: each call copies the arguments into the frame of its own run, which outlives this lambda.
    return [action = &self(), given = std::tuple<Ts...>{std::move(args)...}] {
      return action->run_with(given, detail::untold::thrown);
    };
  }

private:
  [[nodiscard]] Action &self() noexcept { return static_cast<Action &>(*this); }
};

} // namespace detail

/**
 * @brief A function a handler of a component runs, once at a time by default, what it gives written into a target
 * state, as an effect's: whether it is under way, and why it failed, for a button to wait on and a view to show.
 *
 * @code
 * struct product_detail {
 *   state<std::optional<std::string>> name;
 *   async_state<> saving;
 *
 *   // After a success, as React Query's onSuccess: what is typed is forgotten.
 *   action<&products_client::update> save{saving, [this](products::product const &) { name = std::nullopt; }};
 *
 *   element view(props const &given) {
 *     return button(saving->loading ? "Saving…" : "Save") | disabled(saving->loading) |
 *            on_click(save.with(given.item.id, changes()));
 *   }
 * };
 * @endcode
 *
 * `Function` gives its signature: a free function returning a task, as `&products_client::update`, or a member
 * function of a context, given its `shared<C>`, whose work then goes on as the component goes, or of the component
 * itself, given `this`. The target, a state of the same component declared before it, receives what an effect's does:
 * whether a run is under way, why the last failed, and the values it gave, copied; with none, only its caller hears of
 * it. What follows a success, given as it is made, is told what the run gave, before its caller.
 *
 * run() runs it in the task awaiting it, a handler's or mounted()'s, and gives what it gave or what it threw, as a
 * std::expected, not thrown again; what follows a success throws to it, as code of the handler after the run would.
 * with() gives a handler that runs it, for on_click, whose failure, when its target has no place for it, goes to the
 * app's on_error, as a handler's. Every failure of a run, a lack of memory before it began included, goes to its
 * target when it has an error; a lack of memory making the run itself throws from the call, as any call's. Both take
 * the function's parameters, so that a braced argument builds one. A call while one is under way is refused,
 * run_refused at once, the target left as it is, or, with `overlap::restart`, stops it, so that it writes nothing, even
 * one that ignored the stop and came anyway, and gives run_replaced to its caller. A stop of the caller, as a timeout,
 * ends the run and its caller, as a stop does, the target loading no more.
 *
 * Neither copied nor moved: a run under way refers to it. Its arguments are copied into the run, given to the function
 * as rvalues: none can be a view, which would outlive what it refers to. Thread safety: none, as a state.
 */
template <auto Function, overlap Policy = overlap::refuse>
class action : public detail::action_calls<action<Function, Policy>,
                                           typename detail::action_function<decltype(Function)>::arguments> {
  using traits = detail::action_function<decltype(Function)>;

  static_assert(traits::valid, "quoll::ui::action: its function is a free function or a member function that returns "
                               "a quoll::async::task");

public:
  using value_type = typename traits::result;
  using arguments = typename traits::arguments;
  using owner_type = typename traits::owner;
  using result_type = std::expected<value_type, std::exception_ptr>;
  using success_function = typename detail::success_of<value_type>::type;

private:
  static_assert(detail::kept_as_values<value_type> && detail::kept_as_values<arguments>,
                "quoll::ui::action: its arguments are copied into the run, and its result kept: none can be a view");
  static_assert(!traits::writes_arguments,
                "quoll::ui::action: its function is given copies, which it cannot write back: no parameter can be a "
                "reference to what is not const");

  static constexpr bool free = std::is_void_v<owner_type>;

  using call_type = std::function<async::task<value_type>(arguments &&)>;

public:
  /** @brief Of a free function: its caller alone hears of it. */
  action()
    requires free
      : call_(free_call()) {}

  /** @brief Of a free function, writing into `target`, then telling `on_success`, if any, what it gave. */
  template <class T>
    requires free
  explicit action(state<T> &target, success_function on_success = {})
      : call_(free_call()), target_(target, std::true_type{}), on_success_(std::move(on_success)) {
    detail::check_target<T, value_type, true>();
  }

  /** @brief Of a member function of the context `context`, a shared<C> of the same component. */
  template <class C>
    requires(!free && std::same_as<C, owner_type>)
  explicit action(shared<C> &context) : call_(context_call(context)) {}

  /** @brief See above, writing into `target`, then telling `on_success`, if any, what it gave. */
  template <class C, class T>
    requires(!free && std::same_as<C, owner_type>)
  action(shared<C> &context, state<T> &target, success_function on_success = {})
      : call_(context_call(context)), target_(target, std::true_type{}), on_success_(std::move(on_success)) {
    detail::check_target<T, value_type, true>();
  }

  /** @brief Of a member function of the component itself, `self`. */
  template <class C>
    requires(!free && std::same_as<C, owner_type>)
  explicit action(C *self) : call_(self_call(self)) {}

  /** @brief See above, writing into `target`, then telling `on_success`, if any, what it gave. */
  template <class C, class T>
    requires(!free && std::same_as<C, owner_type>)
  action(C *self, state<T> &target, success_function on_success = {})
      : call_(self_call(self)), target_(target, std::true_type{}), on_success_(std::move(on_success)) {
    detail::check_target<T, value_type, true>();
  }

  action(action const &) = delete;
  action(action &&) = delete;
  action &operator=(action const &) = delete;
  action &operator=(action &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~action() { assert(slot_.frames == 0 && "quoll::ui::action: destroyed while a run is under way"); }

private:
  friend class detail::action_calls<action, arguments>;

  [[nodiscard]] static call_type free_call() {
    return [](arguments &&given) {
      return std::apply([](auto &&...values) { return std::invoke(Function, std::move(values)...); }, std::move(given));
    };
  }

  template <class C>
  [[nodiscard]] static call_type context_call(shared<C> &context) {
    return [&context](arguments &&given) {
      return std::apply([&context](auto &&...values) { return context.run(Function, std::move(values)...); },
                        std::move(given));
    };
  }

  template <class C>
  [[nodiscard]] static call_type self_call(C *self) {
    return [self](arguments &&given) {
      return std::apply([self](auto &&...values) { return std::invoke(Function, *self, std::move(values)...); },
                        std::move(given));
    };
  }

  // Not a coroutine: run_in's frame, made by this call, holds the arguments, and every failure of the run goes through
  // tell_failure(), a lack of memory making it thrown from the call, as any call's. A failure its target does not show
  // goes where `failures` says, thrown for a run from an event, which drops what it gives.
  [[nodiscard]] async::task<result_type> run_with(arguments given, detail::untold failures = detail::untold::given) {
    // Called once the run before is stopped, or not at all once refused: what it throws is this run's failure.
    auto make = [this, given = std::move(given)]() mutable { return call_(std::move(given)); };

    auto ask = [this]() -> std::optional<std::stop_token> {
      if (Policy == overlap::refuse && slot_.pending) {
        return std::nullopt;
      }

      return detail::ask(slot_);
    };

    // What follows a success, before its caller: what it throws is thrown to the caller, as what a handler runs after
    // the run would be.
    auto follow_up = [this](auto const &...value) {
      if (on_success_) {
        on_success_(value...);
      }
    };

    return detail::run_in<value_type>(slot_, target_, std::move(make), std::move(ask), failures, follow_up);
  }

  call_type call_;
  detail::target_ref<value_type> target_{};
  success_function on_success_{};
  detail::run_slot slot_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_ACTION_HPP
