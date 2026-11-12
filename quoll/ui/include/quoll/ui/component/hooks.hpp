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

#ifndef QUOLL_UI_COMPONENT_HOOKS_HPP
#define QUOLL_UI_COMPONENT_HOOKS_HPP

#include <quoll/async/task.hpp>
#include <quoll/ui/context/provide.hpp>
#include <quoll/ui/context/provided.hpp>
#include <quoll/ui/context/shared.hpp>
#include <quoll/ui/data/action.hpp>
#include <quoll/ui/data/async_state.hpp>
#include <quoll/ui/data/effect.hpp>
#include <quoll/ui/data/form_state.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/component/hook_store.hpp>
#include <quoll/ui/detail/context/bind_contexts.hpp>
#include <quoll/ui/detail/context/context_member.hpp>
#include <quoll/ui/detail/data/effect_function.hpp>
#include <quoll/ui/detail/view/view_context.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui {

namespace detail {

// What use_state() gives: the state itself, which `auto x = use_state()` would copy, unlinked, were it a state<T>:
// neither copied nor moved, so that only `auto &x`, a reference to what the component holds as long as it lives,
// compiles. An effect, an action, a shared<T> and a provided<T> are neither copied nor moved already.
template <class H>
class hooked final : public H {
public:
  using H::H;

  hooked(hooked const &) = delete;
  hooked(hooked &&) = delete;
  hooked &operator=(hooked const &) = delete;
  hooked &operator=(hooked &&) = delete;

  ~hooked() = default;

  using H::operator=;
};

template <class T>
inline constexpr bool is_tuple = false;

template <class... T>
inline constexpr bool is_tuple<std::tuple<T...>> = true;

// A closure a view wrote, as an effect's function or what follows an action's success, and the props of that view,
// which it may capture by reference: kept together, as the closure shown, and by each run that calls it.
// Thread safety: none, as its app; shared by the runs of one app's executor.
template <class F>
struct written {
  // First, so that it goes after the closure, which may refer to it as it goes.
  std::shared_ptr<void const> props;
  F function;
};

// What the view running now writes.
template <class F>
[[nodiscard]] std::shared_ptr<written<F>> write_now(F function) {
  return std::make_shared<written<F>>(written<F>{current_props(), std::move(function)});
}

// An effect of a function component, its dependencies, states of its own that no app hears of, which each view sets to
// what it gives, as the effect compares a struct's states after the view, and its function, which a view writes and
// which takes effect once that view shows, as React's effects are those of the render committed. A run calls the
// function shown as it is asked for, with its view's props: a view shown during the run changes nothing of it.
// Thread safety: none, as its app.
template <class F, class... D>
class effect_hook {
public:
  using value_type = effect_value_t<F, D...>;

  template <class... Target>
  effect_hook(std::tuple<D...> given, F function, effect_options options, Target &...target)
      : dependencies_(std::move(given)), written_(write(std::move(function), std::nullopt)),
        run_(
            std::apply([](auto &...each) { return std::tie(each...); }, dependencies_), forwarder{this}, target...,
            options
        ) {}

  effect_hook(effect_hook const &) = delete;
  effect_hook(effect_hook &&) = delete;
  effect_hook &operator=(effect_hook const &) = delete;
  effect_hook &operator=(effect_hook &&) = delete;

  ~effect_hook() = default;

  // A view after the first: its dependencies and the function it writes, which take effect once it shows.
  void again(std::tuple<D...> given, F function) { written_ = write(std::move(function), std::move(given)); }

  [[nodiscard]] effect &run() noexcept { return run_; }

  // The effect only: its dependencies change as the view gives them.
  void link(state_link to) noexcept { run_.link(to); }

  // Its view showed: what it wrote, its function and its dependencies, is what runs asked from now on call and see;
  // moved here as a pointer, which cannot throw. One that threw, or whose render failed, shows none.
  void committed() noexcept {
    if (written_) {
      shown_ = std::move(written_);
    }
  }

  // A view starts: what one before it wrote and did not show goes.
  void unwritten() noexcept { written_.reset(); }

  // After a view: the dependencies the last view that showed gave are set into its states, where what throws is the
  // effect's failure to start, as a struct's effect copying its states; then it runs if they changed. None runs before
  // a view that wrote its function showed, as one made in a view that threw.
  void after_view(effect_starter const &start) {
    // None shown yet, or failed to start: it waits, as a struct's effect does, for a view that shows, or for reload().
    if (!shown_ || effect_access::failed(run_)) {
      return;
    }

    if (shown_->dependencies) {
      try {
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
          (std::get<Index>(dependencies_).set(std::move(std::get<Index>(*shown_->dependencies))), ...);
        }(std::index_sequence_for<D...>{});
      } catch (...) {
        shown_->dependencies.reset();
        effect_access::fail(run_, std::current_exception());
      }

      shown_->dependencies.reset();
    }

    run_.after_view(start);
  }

private:
  // What a view writes: its function with its props, and the dependencies it gives, none for the first, which the
  // states hold already.
  struct written_effect : written<F> {
    std::optional<std::tuple<D...>> dependencies;
  };

  [[nodiscard]] static std::shared_ptr<written_effect> write(F function, std::optional<std::tuple<D...>> given) {
    return std::make_shared<written_effect>(written_effect{{current_props(), std::move(function)}, std::move(given)});
  }

  // What the effect calls. The effect copies it once for each run it asks for, as its own: the copy takes what is shown
  // then, and keeps it as it is moved on, so that a run calls the function of the view shown as it was asked for.
  class forwarder {
  public:
    explicit forwarder(effect_hook const *hook) noexcept : hook_(hook) {}

    forwarder(forwarder const &other) noexcept
        : hook_(other.hook_), written_(other.written_ ? other.written_ : other.hook_->shown_) {}

    forwarder(forwarder &&other) noexcept : hook_(other.hook_), written_(std::move(other.written_)) {}

    forwarder &operator=(forwarder const &) = delete;
    forwarder &operator=(forwarder &&) = delete;

    ~forwarder() = default;

    [[nodiscard]] async::task<value_type> operator()(typename dependency_traits<D>::value_type... values) const {
      // A run's own copy, taken as it was asked for, once a view showed; were the effect's own called, it calls the one
      // shown, which a run, started only once a view showed, is not without.
      return call(written_ ? written_ : hook_->shown_, std::move(values)...);
    }

  private:
    // Calls what `kept` holds, kept alive by this frame, with its view's props, as long as the task it gives runs.
    template <class... A>
    static async::task<value_type> call(std::shared_ptr<written_effect> kept, A... args) {
      if constexpr (std::is_void_v<value_type>) {
        co_await std::invoke(kept->function, std::move(args)...);
      } else {
        co_return co_await std::invoke(kept->function, std::move(args)...);
      }
    }

    effect_hook const *hook_;
    std::shared_ptr<written_effect> written_;
  };

  std::tuple<state<D>...> dependencies_;
  // What the view under way wrote, none once it showed, and what the last view that showed wrote.
  std::shared_ptr<written_effect> written_;
  std::shared_ptr<written_effect> shown_;
  effect run_;
};

// An action of a function component, and what follows its successes as its view writes it, which takes effect once
// that view shows, with that view's props, as an effect's function does: a run tells the one it started with.
// Thread safety: none, as its app.
template <auto Function, overlap Policy>
class action_hook final : public action<Function, Policy> {
  using base = action<Function, Policy>;

public:
  using success_function = typename base::success_function;
  using base::base;

  action_hook(action_hook const &) = delete;
  action_hook(action_hook &&) = delete;
  action_hook &operator=(action_hook const &) = delete;
  action_hook &operator=(action_hook &&) = delete;

  ~action_hook() = default;

  // What the view under way writes, with its props, made here, where a lack of memory may throw, and taken as it shows.
  void write(success_function on_success) {
    if (on_success) {
      // The closure, kept with its view's props by the same pointer.
      auto kept = write_now(std::move(on_success));

      written_.emplace(std::shared_ptr<success_function>{kept, &kept->function});
    } else {
      written_.emplace();
    }
  }

  void committed() noexcept {
    if (written_) {
      action_access::follow_success(*this, *written_);
      written_.reset();
    }
  }

  // A view starts: what one before it wrote and did not show goes.
  void unwritten() noexcept { written_.reset(); }

private:
  // What the view under way wrote, as it shows: none written, or what follows a success, which may be nothing.
  std::optional<std::shared_ptr<success_function>> written_;
};

template <class F, class... D, class... Target>
effect &use_effect_at(
    std::source_location const &where, std::tuple<D...> dependencies, F function, effect_options options,
    Target &...target
) {
  using hook_type = effect_hook<F, D...>;

  // Refused at each view, as a struct's effect refuses it as it is made, rather than called after it.
  if constexpr (requires { function == nullptr; }) {
    if (function == nullptr) {
      throw std::invalid_argument{"quoll::ui::use_effect: no function"};
    }
  }

  auto &store = hook_store::current();
  bool made = false;
  auto &hook = store.find<hook_type>(
      where,
      // Called only when none was made there, which leaves them to the branch below otherwise.
      [&] { return std::make_unique<hook_type>(std::move(dependencies), std::move(function), options, target...); },
      made
  );

  if (made) {
    // Its component's first view: the target is loading now if its first run will start, as a struct's effect says
    // before it. Made in a later view, as in a branch, its target is already shown, and linked: it is loading as the
    // run starts, after the view, rather than in the middle of it.
    if (store.first_view()) {
      hook.run().after_view(effect_starter{.first = true});
    }
  } else {
    hook.again(std::move(dependencies), std::move(function));
  }

  return hook.run();
}

} // namespace detail

/**
 * @brief A state of the function component whose view calls it, `initial` its value the first time: the same state at
 * each view, found by where it is called. Write `auto &count = use_state(0);`, and capture `&count`: it lives as long
 * as the component, as a member of a struct.
 */
template <class T>
[[nodiscard]] detail::hooked<state<T>> &
use_state(T initial = T{}, std::source_location where = std::source_location::current()) {
  bool made = false;

  return detail::hook_store::current().find<detail::hooked<state<T>>>(
      where, [&] { return std::make_unique<detail::hooked<state<T>>>(std::move(initial)); }, made
  );
}

/** @brief An async_state of the function component whose view calls it, as use_state(). */
template <class T = void>
[[nodiscard]] detail::hooked<async_state<T>> &
use_async_state(std::source_location where = std::source_location::current()) {
  bool made = false;

  return detail::hook_store::current().find<detail::hooked<async_state<T>>>(
      where, [] { return std::make_unique<detail::hooked<async_state<T>>>(); }, made
  );
}

/**
 * @brief An effect of the function component whose view calls it, on `dependency`, a value its view gives, as React's
 * useEffect(…, [dependency]): after each view in which it changed, the run before stops and `function` runs with it,
 * what it gives written into `target`, a state of the component. As React's, a run calls the function of the view that
 * asked for it, once shown, whose props it keeps: what it captures is never stale, nor gone. The target is the one of
 * its first view.
 */
template <class D, class F, class T>
  requires(!detail::is_tuple<D>) && detail::EffectFunction<F, D>
effect &use_effect(
    D dependency, F function, state<T> &target, effect_options options = {},
    std::source_location where = std::source_location::current()
) {
  return detail::use_effect_at(where, std::tuple<D>{std::move(dependency)}, std::move(function), options, target);
}

/** @brief use_effect() on several values, as `std::tuple{filter, page}`, given to the function in that order. */
template <class... D, class F, class T>
  requires detail::EffectFunction<F, D...>
effect &use_effect(
    std::tuple<D...> dependencies, F function, state<T> &target, effect_options options = {},
    std::source_location where = std::source_location::current()
) {
  return detail::use_effect_at(where, std::move(dependencies), std::move(function), options, target);
}

/** @brief use_effect() on nothing: it runs once, after the component's first view, and stops as the component goes. */
template <class F, class T>
  requires detail::EffectFunction<F>
effect &use_effect(
    F function, state<T> &target, effect_options options = {},
    std::source_location where = std::source_location::current()
) {
  return detail::use_effect_at(where, std::tuple<>{}, std::move(function), options, target);
}

/** @brief use_effect(), for a function that gives nothing. */
template <class D, class F>
  requires(!detail::is_tuple<D>) && detail::EffectFunction<F, D>
effect &use_effect(
    D dependency, F function, effect_options options = {}, std::source_location where = std::source_location::current()
) {
  return detail::use_effect_at(where, std::tuple<D>{std::move(dependency)}, std::move(function), options);
}

/** @brief use_effect() on several values, for a function that gives nothing. */
template <class... D, class F>
  requires detail::EffectFunction<F, D...>
effect &use_effect(
    std::tuple<D...> dependencies, F function, effect_options options = {},
    std::source_location where = std::source_location::current()
) {
  return detail::use_effect_at(where, std::move(dependencies), std::move(function), options);
}

/** @brief use_effect() on nothing, for a function that gives nothing: once, after the first view. */
template <class F>
  requires detail::EffectFunction<F>
effect &
use_effect(F function, effect_options options = {}, std::source_location where = std::source_location::current()) {
  return detail::use_effect_at(where, std::tuple<>{}, std::move(function), options);
}

/**
 * @brief An action of the function component whose view calls it, of a free function: see action. `on_success`, if
 * any, is the one of the last view that showed, as an effect's function: a run tells the one it started with.
 */
template <auto Function, overlap Policy = overlap::refuse>
[[nodiscard]] action<Function, Policy> &use_action(
    typename action<Function, Policy>::success_function on_success = {},
    std::source_location where = std::source_location::current()
)
  requires std::is_void_v<typename action<Function, Policy>::owner_type>
{
  using hook_type = detail::action_hook<Function, Policy>;

  bool made = false;
  auto &found =
      detail::hook_store::current().find<hook_type>(where, [] { return std::make_unique<hook_type>(); }, made);

  found.write(std::move(on_success));
  return found;
}

/**
 * @brief An action of the function component whose view calls it, of a member function of the context `context`, as
 * use_context() gives it: its work goes on as the component goes. See action.
 */
template <auto Function, overlap Policy = overlap::refuse, class C>
[[nodiscard]] action<Function, Policy> &use_action(
    shared<C> &context, typename action<Function, Policy>::success_function on_success = {},
    std::source_location where = std::source_location::current()
)
  requires std::same_as<C, typename action<Function, Policy>::owner_type>
{
  using hook_type = detail::action_hook<Function, Policy>;

  bool made = false;
  auto &found = detail::hook_store::current().find<hook_type>(
      where, [&context] { return std::make_unique<hook_type>(context); }, made
  );

  found.write(std::move(on_success));
  return found;
}

/**
 * @brief A form of the function component whose view calls it, editing `value`, what its view gives, at each view, as
 * a form_state takes a field of its struct's props: see basic_form_state. Write
 * `auto &form = use_form(given.item);`, and give `save.with(form)` to a button.
 */
template <class T>
[[nodiscard]] basic_form_state<T> &
use_form(T const &value, std::source_location where = std::source_location::current()) {
  bool made = false;
  auto &found = detail::hook_store::current().find<basic_form_state<T>>(
      where, [] { return std::make_unique<basic_form_state<T>>(); }, made
  );

  found.take(value);
  return found;
}

/**
 * @brief The context of type T, found by its type among those the components above provide, nearest first, and those
 * of its app, as a struct's `shared<T>`. Write `auto &api = use_context<products_api>();`.
 * @throws std::logic_error for a context none provides.
 */
template <class T>
[[nodiscard]] shared<T> &use_context(std::source_location where = std::source_location::current()) {
  auto &store = detail::hook_store::current();
  bool made = false;
  // Found as it is first called, before the store keeps it: one none provides is not kept.
  auto &found = store.find<detail::hooked<shared<T>>>(
      where,
      [&store] {
        auto made_hook = std::make_unique<detail::hooked<shared<T>>>();

        made_hook->bind(store.finder());
        return made_hook;
      },
      made
  );

  // What it sees counted from now, as the view under way reads it.
  if (made) {
    detail::context_access::mark(found);
  }

  return found;
}

/**
 * @brief A context of type T of the function component whose view calls it, made by it, which the components of its
 * view find before those above, as a struct's `provided<T>`: as React's Context.Provider. The context finds those it
 * uses as its component would.
 */
template <class T>
[[nodiscard]] provided<T> &provide_context(std::source_location where = std::source_location::current()) {
  auto &store = detail::hook_store::current();
  bool made = false;
  auto &found = store.find<detail::hooked<provided<T>>>(
      where,
      [&store] {
        auto const finder = store.finder();

        // What it uses is there before it is made, as for a struct's provided<T>: one refused is never made, so that
        // its destructor, which may read what it found, does not run.
        detail::refuse_missing(detail::context_table_of<T>.finds, finder);

        auto made_hook = std::make_unique<detail::hooked<provided<T>>>();

        made_hook->bind(finder);
        return made_hook;
      },
      made
  );

  if (made) {
    detail::context_access::mark(found);
  }

  return found;
}

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_HOOKS_HPP
