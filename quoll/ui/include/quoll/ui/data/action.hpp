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

#ifndef QUOLL_UI_DATA_ACTION_HPP
#define QUOLL_UI_DATA_ACTION_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/data/async_state.hpp>
#include <quoll/ui/data/run_error.hpp>
#include <quoll/ui/detail/data/run_slot.hpp>
#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/detail/data/target.hpp>

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

template <auto Member>
class form_state;

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

// What an action holds of what its runs give: a copy, for a value it can make empty and copy; nothing for another, as a
// move-only one, which only run()'s caller is given. One held moves in without throwing, which its check says.
template <class V>
struct held_by_action {
  using type = std::conditional_t<std::copy_constructible<V> && std::default_initializable<V>, V, void>;
};

template <>
struct held_by_action<void> {
  using type = void;
};

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
   * @brief Runs it, as its overlap says: what it gave, or what it threw, as it then holds them; run_refused or
   * run_replaced for a call that gave no result of its own.
   */
  [[nodiscard]] auto run(Ts... args) QUOLL_LIFETIMEBOUND {
    return self().run_with(std::tuple<Ts...>{std::move(args)...}, detail::failure_to::caller);
  }

  /**
   * @brief A handler running it with copies of `args`, for on_click: what it gave and why it failed are what it holds,
   * and a failure reaches the app's on_error too, as a handler's; what it gave goes to what follows a success. A
   * handler going on with the result awaits run(), whose caller alone is given a failure.
   */
  [[nodiscard]] auto with(Ts... args) QUOLL_LIFETIMEBOUND {
    static_assert(
        (std::copy_constructible<Ts> && ...),
        "quoll::ui::action: with() copies its arguments for each click: they are copyable"
    );

    // Not a coroutine: each call copies the arguments into the frame of its own run, which outlives this lambda.
    return [action = &self(), given = std::tuple<Ts...>{std::move(args)...}] {
      return action->run_with(given, detail::failure_to::app);
    };
  }

  /**
   * @brief A handler running it with what `form` edits, its value() as it is at each click, for a function taking it:
   * a button sends the form as it stands, as with() the values it is given. A click while the form is not valid is
   * refused, as one while a run is under way: nothing runs, and its runs are left as they are.
   */
  template <auto Member>
    requires(sizeof...(Ts) == 1 && (std::same_as<typename form_state<Member>::value_type, Ts> && ...))
  [[nodiscard]] auto with(form_state<Member> const &form QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND {
    return [action = &self(), form = &form] {
      return action->run_with(std::tuple<Ts...>{form->value()}, detail::failure_to::app, form->valid());
    };
  }

private:
  [[nodiscard]] Action &self() noexcept { return static_cast<Action &>(*this); }
};

} // namespace detail

/**
 * @brief A function a handler of a component runs, once at a time by default, holding where its runs are as an
 * async_state does: whether one is under way, why the last failed, and what it gave, for a button to wait on and a view
 * to show, as React Query's useMutation.
 *
 * @code
 * struct product_detail {
 *   shared<catalog> products;
 *   action<&catalog::save> save{products};
 *
 *   element view(props const &given) {
 *     return button(save->loading ? "Saving…" : "Save") | disabled(save->loading) |
 *            on_click(save.with(given.item.id, changes(given.item)));
 *   }
 * };
 * @endcode
 *
 * `Function` gives its signature: a free function returning a task, as `&products_client::update`, or a member
 * function of a context, given its `shared<C>`, whose work then goes on as the component goes, or of the component
 * itself, given `this`, declared before it. It is read as a state: `save->loading`, `save->error` and `save->value`,
 * what the last run that succeeded gave, copied, none for a value it cannot copy or make empty, as a move-only one,
 * which only run()'s caller is given; a change of them shows its component again. What follows a success, given as it
 * is made, as React Query's onSuccess, is told what the run gave, before its caller.
 *
 * run() runs it in the task awaiting it, a handler's or mounted()'s, and gives what it gave or what it threw, as a
 * std::expected, not thrown again; what follows a success throws to it, as code of the handler after the run would.
 * with() gives a handler that runs it, for on_click. Every failure of a run, a lack of memory before it began included,
 * is its error, and, from with(), the app's on_error's too, as every failure of an effect or a handler; a lack of
 * memory making the run itself throws from the call, as any call's. Both take the function's parameters, so that a
 * braced argument builds one. A call while one is under way is refused, run_refused at once, where its runs are left as
 * they are, or, with `overlap::restart`, stops it, so that it writes nothing, even one that ignored the stop and came
 * anyway, and gives run_replaced to its caller. A stop of the caller, as a timeout, ends the run and its caller, as a
 * stop does, loading no more.
 *
 * Neither copied nor moved: a run under way refers to it. Its arguments are copied into the run, given to the function
 * as rvalues: none can be a view, which would outlive what it refers to. Thread safety: none, as a state.
 */
template <auto Function, overlap Policy = overlap::refuse>
class action : public detail::action_calls<
                   action<Function, Policy>, typename detail::action_function<decltype(Function)>::arguments> {
  using traits = detail::action_function<decltype(Function)>;

  static_assert(
      traits::valid, "quoll::ui::action: its function is a free function or a member function that returns "
                     "a quoll::async::task"
  );

public:
  using value_type = typename traits::result;
  using arguments = typename traits::arguments;
  using owner_type = typename traits::owner;
  using result_type = std::expected<value_type, std::exception_ptr>;
  using success_function = typename detail::success_of<value_type>::type;
  // What it holds of what its runs give, as `->value`: none for a value it cannot copy or make empty.
  using held_type = typename detail::held_by_action<value_type>::type;

private:
  static_assert(
      detail::kept_as_values<value_type> && detail::kept_as_values<arguments>,
      "quoll::ui::action: its arguments are copied into the run, and its result kept: none can be a view"
  );
  static_assert(
      !traits::writes_arguments,
      "quoll::ui::action: its function is given copies, which it cannot write back: no parameter can be a "
      "reference to what is not const"
  );

  static constexpr bool free = std::is_void_v<owner_type>;

  // What it holds, checked once: a result that moves without throwing, so that a run ends on what it holds.
  static constexpr bool holds_checked = (detail::check_target<async_value<held_type>, value_type, true>(), true);
  static_assert(holds_checked);

  using call_type = std::function<async::task<value_type>(arguments &&)>;

public:
  /** @brief Of a free function. */
  action()
    requires free
      : call_(free_call()) {}

  /** @brief Of a free function, telling `on_success` what it gave, before its caller. */
  explicit action(success_function on_success)
    requires free
      : call_(free_call()), on_success_(std::move(on_success)) {}

  /**
   * @brief Of a member function of the context `context`, a shared<C> of the same component, its work going on as the
   * component goes; then telling `on_success`, if any, what it gave.
   */
  template <class C>
    requires(!free && std::same_as<C, owner_type>)
  explicit action(shared<C> &context, success_function on_success = {})
      : call_(context_call(context)), on_success_(std::move(on_success)) {}

  /** @brief Of a member function of the component itself, `self`, declared before it; then telling `on_success`. */
  template <class C>
    requires(!free && std::same_as<C, owner_type>)
  explicit action(C *self, success_function on_success = {})
      : call_(self_call(self)), on_success_(std::move(on_success)) {}

  action(action const &) = delete;
  action(action &&) = delete;
  action &operator=(action const &) = delete;
  action &operator=(action &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~action() { assert(slot_.frames == 0 && "quoll::ui::action: destroyed while a run is under way"); }

  /** @brief Where its runs are, as an async_state holds it: under way, why the last failed, and what it gave. */
  [[nodiscard]] async_value<held_type> const &operator*() const noexcept QUOLL_LIFETIMEBOUND { return *state_; }

  /** @brief See operator*(): `save->loading`, `save->error`, `save->value`. */
  [[nodiscard]] async_value<held_type> const *operator->() const noexcept QUOLL_LIFETIMEBOUND {
    return state_.operator->();
  }

  /** @brief For the app of the component holding it, as a state's: a change of where its runs are shows it again. */
  void link(detail::state_link to) noexcept { state_.link(to); }

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
      return std::apply(
          [&context](auto &&...values) { return context.run(Function, std::move(values)...); }, std::move(given)
      );
    };
  }

  template <class C>
  [[nodiscard]] static call_type self_call(C *self) {
    return [self](arguments &&given) {
      return std::apply(
          [self](auto &&...values) { return std::invoke(Function, *self, std::move(values)...); }, std::move(given)
      );
    };
  }

  // Not a coroutine: run_in's frame, made by this call, holds the arguments, and every failure of the run goes through
  // tell_failure(), into what it holds, and to the app's on_error for `failure_to::app`, with()'s; a lack of memory
  // making it throws from the call, as any call's.
  // A run not `allowed`, as of a form not valid, is refused, as one while another is under way with overlap::refuse.
  [[nodiscard]] async::task<result_type> run_with(arguments given, detail::failure_to failures, bool allowed = true) {
    // Called once the run before is stopped, or not at all once refused: what it throws is this run's failure.
    auto make = [this, given = std::move(given)]() mutable { return call_(std::move(given)); };

    auto ask = [this, allowed]() -> std::optional<std::stop_token> {
      if (!allowed || (Policy == overlap::refuse && slot_.pending)) {
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

  // Where its runs are, before what writes into it.
  async_state<held_type> state_;
  call_type call_;
  detail::target_ref<value_type> target_{state_, std::true_type{}};
  success_function on_success_{};
  detail::run_slot slot_;
};

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_ACTION_HPP
