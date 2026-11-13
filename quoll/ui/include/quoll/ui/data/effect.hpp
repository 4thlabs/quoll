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

#ifndef QUOLL_UI_DATA_EFFECT_HPP
#define QUOLL_UI_DATA_EFFECT_HPP

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/context/member_join.hpp>
#include <quoll/ui/detail/data/effect_function.hpp>
#include <quoll/ui/detail/data/effect_member.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/props_follow.hpp>
#include <quoll/ui/detail/data/run_slot.hpp>
#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/detail/data/target.hpp>

#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui {

class effect;

namespace detail {

// What a component's use_effect() reaches of an effect: failing as it does when what it starts with throws,
// and whether it did, as it then waits for reload().
struct effect_access {
  // The run before stops, the target shows `failure`, and the effect waits for reload(); then thrown, for on_error.
  [[noreturn]] static void fail(effect &run, std::exception_ptr const &failure);

  [[nodiscard]] static bool failed(effect const &run) noexcept;
};

// Runs an effect as a task of its component, unless the component stopped since it was started, as one that went with
// its app before its executor ran: an effect does nothing once its component went. Every failure goes where a handler's
// does, through tell_failure(), its target showing it, as a lack of memory making the run does. A coroutine of its own,
// so that what it needs is in its frame.
template <class V, class Make>
async::task<> run_effect(run_slot &slot, target_ref<V> target, Make make, std::stop_token mine) {
  auto const stop = co_await async::current_stop_token();

  if (stop.stop_requested()) {
    co_return;
  }

  auto const asked = [mine] { return std::optional<std::stop_token>{mine}; };
  std::optional<async::task<std::expected<V, std::exception_ptr>>> running;

  try {
    running.emplace(run_in<V>(slot, target, std::move(make), asked, failure_to::app));
  } catch (...) {
    // As run_in would: a run a later one replaced is no failure.
    if (!mine.stop_requested()) {
      tell_failure(target.hooks(), failure_to::app, std::current_exception());
    }

    co_return;
  }

  static_cast<void>(co_await std::move(*running));
}

// What `make` gives, once `pause` has passed: a coroutine of its own, so that the wait and the call are in its frame,
// and stop with the run.
template <class V, class Make>
async::task<V> after_pause(std::chrono::nanoseconds pause, Make make) {
  co_await async::sleep_for(pause);

  if constexpr (std::is_void_v<V>) {
    co_await make();
  } else {
    co_return co_await make();
  }
}

} // namespace detail

/** @brief How an effect runs, given after its function and its target: `{.debounce = 300ms}`. */
struct effect_options {
  /**
   * @brief How long its states stay the same before a run that follows a change starts, as a search waits for what
   * is typed: each change starts the wait again. None by default.
   */
  std::chrono::nanoseconds debounce{0};
};

/**
 * @brief What runs as states change, as React's useEffect with them as its dependencies: after each view in which one
 * changed, the run before stopped, what the function gives written into a target state. What use_effect() gives a
 * component, a member of a context, which loads what the app's components share, or a member of a component written as
 * a struct, which may also follow its props: a field, `effect load{&props::id, f, target};`, or a function of them,
 * `effect load{[](props const &given) { return given.item.id; }, f, target};`.
 *
 * @code
 * struct catalog {
 *   // The products, whether they are loading, and why they did not load.
 *   async_state<std::vector<products::product>> items;
 *
 *   effect load{std::tie(), products_client::list, items};
 * };
 * @endcode
 *
 * A member of a context, after the states it depends on: one, or several as `std::tie(filter, page)`, or none as
 * `std::tie()`, which runs it once. After a view of the app, once it is shown, as React runs its effects once a
 * render is committed, the effect compares those states with their values at its last run, with `==`: one changed,
 * the run before is stopped, so that it writes nothing, even one that ignored the stop and came anyway, and the
 * function runs, as a task of the app, or of the component whose hook it is, with a copy of each value, as rvalues. A
 * state holding a `std::optional` gives what it holds, and while one holds none, nothing runs, the run before stopped.
 * reload() runs it again, after the view it asks for, as a change would: one way to run it, as React has. Its options,
 * last, may debounce it, as
 * `{.debounce = 300ms}` for a search on what is typed: a run that follows a change waits that long before it calls the
 * function, its target loading, and the next change stops it there, as any; the first run that starts, as once an
 * optional holds a value, and one reload() asks for do not wait.
 *
 * The function returns a task of nothing, of a value, or of a `std::tuple` of values, as `when_all` gives several
 * loads, which go into the target, a state of the same context, declared before it: what each of its elements
 * receives goes by its type. A `bool` is whether a run is under way, a `std::exception_ptr` why the last failed, and
 * the others the values, in order; an `async_state` names them `loading`, `error` and `value`. A target holding no
 * tuple holds one of these. The target is loading from the start, as React's `useState(true)`, when its first run
 * will start, its states holding a value: the first view of the app comes before the
 * run; a view written once, with to_html, which runs no effect, shows it loading. As a run starts, its target is
 * loading and has no error, the values before kept; as it ends, the target holds all it gave at once, or its failure,
 * the values before kept. Every failure reaches the app's on_error too, shown or not, as a handler's, and an effect
 * with no target runs a function that gives nothing, such as one that saves what is typed. The values of a target read
 * by type cannot be a bool or an error, which it would not tell apart from its flags; an async_state's value is what
 * the function gives, whole, whatever it is. A debounce of less than nothing throws std::invalid_argument.
 *
 * Neither copied nor moved: a run under way refers to it, and a component stays where it was made. The values of its
 * states are values: a reference or a view would outlive what it refers to, also inside an optional, a pair, a tuple,
 * a variant or an expected; a view in a container or a struct of the program's is not seen. Thread safety: none, as a
 * state.
 */
class effect {
public:
  /**
   * @brief Runs `function` with what `dependency` holds as it changes, for a function that gives nothing.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class D, class F>
    requires detail::EffectFunction<F, D>
  effect(state<D> &dependency, F function, effect_options options = {})
      : effect(std::tie(dependency), std::move(function), options) {}

  /**
   * @brief Runs `function` with what `dependency` holds as it changes, what it gives written into `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class D, class F, class T>
    requires detail::EffectFunction<F, D>
  effect(state<D> &dependency, F function, state<T> &target, effect_options options = {})
      : effect(std::tie(dependency), std::move(function), target, options) {}

  /**
   * @brief Runs `function` with what `dependencies`, from std::tie, hold as one changes, for a function that gives
   * nothing.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class... D, class F>
    requires detail::EffectFunction<F, D...>
  effect(std::tuple<state<D> &...> dependencies, F function, effect_options options = {}) {
    using value = detail::effect_value_t<F, D...>;

    static_assert(std::is_void_v<value>, "quoll::ui::effect: the values its function gives go into a target state");

    init<value, D...>(states_reader(dependencies), std::move(function), detail::target_ref<value>{}, options);
  }

  /**
   * @brief Runs `function` with what `dependencies`, from std::tie, hold as one changes, what it gives written into
   * `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class... D, class F, class T>
    requires detail::EffectFunction<F, D...>
  effect(std::tuple<state<D> &...> dependencies, F function, state<T> &target, effect_options options = {}) {
    using value = detail::effect_value_t<F, D...>;

    static_assert(
        detail::kept_as_values<value>, "quoll::ui::effect: the values its function gives are kept, as values"
    );
    detail::check_target<T, value, false>();
    init<value, D...>(
        states_reader(dependencies), std::move(function), detail::target_ref<value>{target, std::false_type{}}, options
    );
  }

  /**
   * @brief Runs `function` with what `follow`, a field of its component's props or a function of them, gives as it
   * changes, what it gives written into `target`: a member of a component written as a struct,
   * `effect load{&props::id, products_client::sales, sales};` or
   * `effect load{[](props const &given) { return given.item.id; }, products_client::sales, sales};`. Read from the
   * props of each view, compared with what the last run was given.
   * @throws std::logic_error outside the making of a struct, or for the props of another.
   * @throws std::invalid_argument for no field or no function, as a null pointer or an empty std::function, rather
   * than at each run.
   */
  template <class Follow, class F, class T>
    requires detail::PropsFollow<Follow> && detail::EffectFunction<F, typename detail::follow_traits<Follow>::value>
  effect(Follow follow, F function, state<T> &target, effect_options options = {}) {
    using followed = typename detail::follow_traits<Follow>::value;
    using value = detail::effect_value_t<F, followed>;

    detail::check_followed<followed>();

    static_assert(
        detail::kept_as_values<value>, "quoll::ui::effect: the values its function gives are kept, as values"
    );
    detail::check_target<T, value, false>();
    init<value, followed>(
        props_reader(follow), std::move(function), detail::target_ref<value>{target, std::false_type{}}, options
    );
  }

  /** @brief See the one with a target: for a function that gives nothing. */
  template <class Follow, class F>
    requires detail::PropsFollow<Follow> && detail::EffectFunction<F, typename detail::follow_traits<Follow>::value>
  effect(Follow follow, F function, effect_options options = {}) {
    using followed = typename detail::follow_traits<Follow>::value;
    using value = detail::effect_value_t<F, followed>;

    detail::check_followed<followed>();

    static_assert(std::is_void_v<value>, "quoll::ui::effect: the values its function gives go into a target state");

    init<value, followed>(props_reader(follow), std::move(function), detail::target_ref<value>{}, options);
  }

  effect(effect const &) = delete;
  effect(effect &&) = delete;
  effect &operator=(effect const &) = delete;
  effect &operator=(effect &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~effect();

  /**
   * @brief Runs it again for the values its states hold now, as a change would: after the view it asks for, the run
   * before stopped, as React runs an effect again. What asking for that view throws goes to the caller, as a state's
   * change does.
   */
  void reload();

  /** @brief For the app, or the component of its hook, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

  /**
   * @brief For the app, or the component of its hook, after each view: runs it if one of its states changed. What
   * fails on the way, as a copy or a start lacking memory, stops the run before all the same, is its target's failure,
   * and is thrown, for the app's on_error; the effect then waits for reload(). Before the first view, it only says
   * whether its target is loading.
   */
  void after_view(detail::effect_starter const &start);

private:
  friend struct detail::effect_access;

  // Its failure to start, which its target shows, and the app's on_error hears of, thrown: then it waits for reload().
  [[noreturn]] void fail_start(std::exception_ptr const &failure);

  // What reads its values, from its states.
  template <class... D>
  [[nodiscard]] static auto states_reader(std::tuple<state<D> &...> dependencies) {
    return [dependencies] { return std::apply([](auto &...held) { return std::tie(*held...); }, dependencies); };
  }

  // What reads what it follows of the props of the struct being made, at each view: its props are replaced as its
  // parent views again, so it keeps where they are, not a reference into them. A field is read by reference, the props
  // alive for the read, as a state's value is; what a function gives, by value.
  template <class Follow>
  [[nodiscard]] auto props_reader(Follow follow) {
    using props = typename detail::follow_traits<Follow>::props;

    // A null pointer, or a function holding none, as an empty std::function.
    if constexpr (detail::may_be_empty<Follow>) {
      if (follow == nullptr) {
        throw std::invalid_argument{"quoll::ui::effect: no prop, nor a function of them"};
      }
    }

    auto const *const slot = detail::making_props_of(async::detail::type_id_of<props>(), this);

    return [slot, follow] {
      auto const &given = *static_cast<props const *>(*slot);

      if constexpr (std::is_member_object_pointer_v<Follow>) {
        return std::tie(given.*follow);
      } else {
        return std::tuple<typename detail::follow_traits<Follow>::value>{std::invoke(follow, given)};
      }
    };
  }

  // `read` gives what it depends on, as a tuple of references to D....
  template <class V, class... D, class Read, class F>
  void init(Read read, F function, detail::target_ref<V> target, effect_options options) {
    static_assert(
        (!detail::holds_view<D> && ...),
        "quoll::ui::effect: its states' values are copied into the run, which a view would outlive"
    );
    static_assert(
        (... && (std::equality_comparable<D> && std::copyable<D>)),
        "quoll::ui::effect: its states' values are compared with ==, and copied"
    );

    using values = std::tuple<typename detail::dependency_traits<D>::value_type...>;

    if constexpr (requires { function == nullptr; }) {
      if (function == nullptr) {
        throw std::invalid_argument{"quoll::ui::effect: no function"};
      }
    }

    if (options.debounce < std::chrono::nanoseconds::zero()) {
      throw std::invalid_argument{"quoll::ui::effect: a debounce of less than nothing"};
    }

    target_ = target.hooks();

    // What the last record took, which a run that follows it copies: what it follows is read once a view, a function
    // of the props called once, and a run is given what was compared.
    auto const last = std::make_shared<std::optional<std::tuple<D...>>>();

    watch_ = [read, last](watching asked) {
      // Forgotten before any comparison, which may be what failed: reload() runs it whatever its states compare.
      if (asked == watching::forget) {
        last->reset();
        return std::optional<watched>{};
      }

      // Before the first view, what holds a value always will: nothing read, which may throw where nothing may.
      constexpr bool always_holds =
          (std::same_as<typename detail::dependency_traits<D>::value_type, std::remove_cv_t<D>> && ...);

      if (asked == watching::peek && always_holds) {
        return std::optional<watched>{watched{.differs = false, .holds = true}};
      }

      auto const now = read();
      bool const holds = std::apply(
          [](auto const &...held) {
            return (detail::dependency_traits<std::remove_cvref_t<decltype(held)>>::holds(held) && ...);
          },
          now
      );

      // Before the first view: whether its first run will start, nothing recorded.
      if (asked == watching::peek) {
        return std::optional<watched>{watched{.differs = false, .holds = holds}};
      }

      bool const differs = !*last || !(**last == now);

      if (differs) {
        last->emplace(now);
      }

      return std::optional<watched>{watched{.differs = differs, .holds = holds}};
    };

    // Called only after a record that found them changed, and took them.
    run_ = [this, last, function = std::move(function), target,
            debounce = options.debounce](std::stop_token mine, bool waits) {
      // Its own copy of each value, given as rvalues: a function taking them by reference refers to the run's copy.
      auto copied = std::apply(
          [](auto const &...held) {
            return values{detail::dependency_traits<std::remove_cvref_t<decltype(held)>>::value(held)...};
          },
          **last
      );

      auto call = [function, copied = std::move(copied)]() mutable {
        return std::apply([&function](auto &...value) { return function(std::move(value)...); }, copied);
      };

      // The function called once the pause has passed, so that a run a change stops meanwhile calls nothing.
      auto make = [call = std::move(call),
                   pause = waits ? debounce : std::chrono::nanoseconds::zero()]() mutable -> async::task<V> {
        if (pause == std::chrono::nanoseconds::zero()) {
          return call();
        }

        return detail::after_pause<V>(pause, std::move(call));
      };

      return detail::run_effect<V>(slot_, target, std::move(make), std::move(mine));
    };
  }

  // The run under way or queued stops, and writes nothing.
  void stop_before() noexcept;

  enum class watching { record, forget, peek };

  struct watched {
    bool differs;
    bool holds;
  };

  // Its states, compared with their values at the last run, which `record` takes as they differ and `forget` drops,
  // and the run, made
  // for their values now: erased, as their types are not the effect's.
  // What `record` saw, none for `forget`.
  std::function<std::optional<watched>(watching)> watch_;
  // `waits` for a run its debounce waits for, one that follows a run started.
  std::function<async::task<>(std::stop_token, bool waits)> run_;
  detail::run_slot slot_;
  detail::target_hooks target_;
  detail::state_link link_{};
  // Failed to start, until reload().
  bool failed_ = false;
  // A run started since it was made or reloaded: the next one waits for its debounce, the first calls at once.
  bool started_ = false;
  // Last: joins the component being made once it is made, if it is one of its members.
  detail::member_join<effect> joined_{*this};
};

static_assert(detail::LinkedMember<effect>);
static_assert(detail::EffectMember<effect>);

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_EFFECT_HPP
