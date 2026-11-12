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

#ifndef QUOLL_UI_EFFECT_HPP
#define QUOLL_UI_EFFECT_HPP

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/effect_function.hpp>
#include <quoll/ui/detail/effect_member.hpp>
#include <quoll/ui/detail/effect_starter.hpp>
#include <quoll/ui/detail/linked_member.hpp>
#include <quoll/ui/detail/run_slot.hpp>
#include <quoll/ui/detail/state_link.hpp>
#include <quoll/ui/detail/target.hpp>
#include <quoll/ui/state.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui {

namespace detail {

// Runs an effect as a task of its component, unless the component stopped since it was started, as one that went with
// its app before its executor ran: an effect does nothing once unmounted() ran. A failure its target has no place for,
// as one without an error, goes where a handler's does. A coroutine of its own, so that what it needs is in its frame.
template <class V, class Make>
async::task<> run_effect(run_slot &slot, target_ref<V> target, Make make, std::stop_token mine, bool takes_errors) {
  auto const stop = co_await async::current_stop_token();

  if (stop.stop_requested()) {
    co_return;
  }

  auto const asked = [mine] { return std::optional<std::stop_token>{mine}; };
  auto const ended = co_await run_in<V>(slot, target, std::move(make), asked);

  // Replaced exactly when its stop was asked for: that run says what is shown.
  if (!ended && !takes_errors && !mine.stop_requested()) {
    std::rethrow_exception(ended.error());
  }
}

} // namespace detail

/**
 * @brief What a component runs as states of its change, as React's useEffect with them as its dependencies: after each
 * of its views in which one changed, the run before stopped, what the function gives written into a target state.
 *
 * @code
 * struct sales_panel {
 *   state<std::int64_t> id;
 *   // The sales, whether they are loading, and why they did not load: loading from the start, as the first view comes
 *   // before the first run.
 *   state<std::tuple<products::monthly_sales, bool, std::exception_ptr>> sales{{{}, true, nullptr}};
 *
 *   effect load{id, products_client::sales, sales};
 *
 *   element view() {
 *     auto const &[monthly, loading, error] = *sales;
 *
 *     return loading ? p("Loading") : component<sales_chart>({.sales = monthly});
 *   }
 * };
 * @endcode
 *
 * A member of a component, after the states it depends on: one, or several as `std::tie(filter, page)`, or none as
 * `std::tie()`, which runs it once. After a view of the component, once it is shown, as React runs its effects once a
 * render is committed, the effect compares those states with their values at its last run, with `==`: one changed,
 * the run before is stopped, so that it writes nothing, even one that ignored the stop and came anyway, and the
 * function runs, as a task of the component, with a copy of each value, as rvalues. A state holding a `std::optional`
 * gives what it holds, and while one holds none, nothing runs, the run before stopped. reload() runs it again, after
 * the view it asks for, as a change would: one way to run it, as React has.
 *
 * The function returns a task of nothing, of a value, or of a `std::tuple` of values, as `when_all` gives several
 * loads, which go into the target, a state of the same component, declared before it: what each of its elements
 * receives goes by its type. A `bool` is whether a run is under way, a `std::exception_ptr` why the last failed, and
 * the others the values, in order. A target holding no tuple holds one of these. As a run starts, its target is loading
 * and has no error, the values before kept; as it ends, the target holds all it gave at once, or its failure, the
 * values before kept. A target with no error leaves a failure to the app's on_error, as a handler's, and an effect
 * with no target runs a function that gives nothing, such as a debounced search. The values cannot be a bool or an
 * error, which the target would not tell apart from its flags.
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
  effect(state<D> &dependency, F function) : effect(std::tie(dependency), std::move(function)) {}

  /**
   * @brief Runs `function` with what `dependency` holds as it changes, what it gives written into `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class D, class F, class T>
    requires detail::EffectFunction<F, D>
  effect(state<D> &dependency, F function, state<T> &target)
      : effect(std::tie(dependency), std::move(function), target) {}

  /**
   * @brief Runs `function` with what `dependencies`, from std::tie, hold as one changes, for a function that gives
   * nothing.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class... D, class F>
    requires detail::EffectFunction<F, D...>
  effect(std::tuple<state<D> &...> dependencies, F function) {
    using value = detail::effect_value_t<F, D...>;

    static_assert(std::is_void_v<value>, "quoll::ui::effect: the values its function gives go into a target state");

    init<value, D...>(states_reader(dependencies), std::move(function), detail::target_ref<value>{}, false, nullptr);
  }

  /**
   * @brief Runs `function` with what `dependencies`, from std::tie, hold as one changes, what it gives written into
   * `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class... D, class F, class T>
    requires detail::EffectFunction<F, D...>
  effect(std::tuple<state<D> &...> dependencies, F function, state<T> &target) {
    using value = detail::effect_value_t<F, D...>;

    static_assert(detail::kept_as_values<value>,
                  "quoll::ui::effect: the values its function gives are kept, as values");
    detail::check_target<T, value, false>();
    init<value, D...>(states_reader(dependencies), std::move(function),
                      detail::target_ref<value>{target, std::false_type{}}, detail::target_roles<T>::error, nullptr);
  }

  /**
   * @brief Runs `function` with what the field `member` of its component's props holds as it changes, as React's
   * useEffect(…, [props.id]), for a function that gives nothing.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class P, class D, class F>
    requires(!std::is_function_v<D>) && detail::EffectFunction<F, D>
  effect(D P::*member, F function) {
    using value = detail::effect_value_t<F, D>;

    static_assert(std::is_void_v<value>, "quoll::ui::effect: the values its function gives go into a target state");

    init<value, D>(props_reader(member), std::move(function), detail::target_ref<value>{}, false,
                   async::detail::type_id_of<P>());
  }

  /**
   * @brief Runs `function` with what the field `member` of its component's props holds as it changes, what it gives
   * written into `target`.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class P, class D, class F, class T>
    requires(!std::is_function_v<D>) && detail::EffectFunction<F, D>
  effect(D P::*member, F function, state<T> &target) {
    using value = detail::effect_value_t<F, D>;

    static_assert(detail::kept_as_values<value>,
                  "quoll::ui::effect: the values its function gives are kept, as values");
    detail::check_target<T, value, false>();
    init<value, D>(props_reader(member), std::move(function), detail::target_ref<value>{target, std::false_type{}},
                   detail::target_roles<T>::error, async::detail::type_id_of<P>());
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

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

  /**
   * @brief For the app of the component holding it, after each of its views: runs it if one of its states changed. What
   * fails on the way, as a copy or a start lacking memory, stops the run before all the same, and is its target's
   * failure, or thrown for a target with no place for it; the effect then waits for reload().
   */
  void after_view(detail::effect_starter const &start);

private:
  // What reads its values, from its states, whatever the props are.
  template <class... D>
  [[nodiscard]] static auto states_reader(std::tuple<state<D> &...> dependencies) {
    return [dependencies](void const *) {
      return std::apply([](auto &...held) { return std::tie(*held...); }, dependencies);
    };
  }

  // What reads its value, from the field `member` of the props of its component, which `props` points to.
  template <class P, class D>
  [[nodiscard]] static auto props_reader(D P::*member) {
    return [member](void const *props) { return std::tie(static_cast<P const *>(props)->*member); };
  }

  // `read` gives what it depends on, from the props of its component, as a tuple of references to D...; `props_type`,
  // none for an effect on states, is the type of the props it reads.
  template <class V, class... D, class Read, class F>
  void init(Read read, F function, detail::target_ref<V> target, bool takes_errors, async::detail::type_id props_type) {
    static_assert((!detail::holds_view<D> && ...),
                  "quoll::ui::effect: its states' values are copied into the run, which a view would outlive");
    static_assert((... && (std::equality_comparable<D> && std::copyable<D>)),
                  "quoll::ui::effect: its states' values are compared with ==, and copied");

    using values = std::tuple<typename detail::dependency_traits<D>::value_type...>;

    if constexpr (requires { function == nullptr; }) {
      if (function == nullptr) {
        throw std::invalid_argument{"quoll::ui::effect: no function"};
      }
    }

    target_ = target.hooks();
    props_type_ = props_type;

    watch_ = [read, last = std::optional<std::tuple<D...>>{}](watching asked, void const *props) mutable {
      // Forgotten before any comparison, which may be what failed: reload() runs it whatever its states compare.
      if (asked == watching::forget) {
        last.reset();
        return std::optional<watched>{};
      }

      auto const now = read(props);
      bool const differs = !last || !(*last == now);
      bool const holds = std::apply(
          [](auto const &...held) {
            return (detail::dependency_traits<std::remove_cvref_t<decltype(held)>>::holds(held) && ...);
          },
          now);

      if (differs) {
        last.emplace(now);
      }

      return std::optional<watched>{watched{.differs = differs, .holds = holds}};
    };

    run_ = [this, read, function = std::move(function), target, takes_errors](std::stop_token mine, void const *props) {
      // Its own copy of each value, given as rvalues: a function taking them by reference refers to the run's copy.
      auto copied = std::apply(
          [](auto const &...held) {
            return values{detail::dependency_traits<std::remove_cvref_t<decltype(held)>>::value(held)...};
          },
          read(props));

      auto make = [function, copied = std::move(copied)]() mutable {
        return std::apply([&function](auto &...value) { return function(std::move(value)...); }, copied);
      };

      return detail::run_effect<V>(slot_, target, std::move(make), std::move(mine), takes_errors);
    };
  }

  // The run under way or queued stops, and writes nothing.
  void stop_before() noexcept;

  enum class watching { record, forget };

  struct watched {
    bool differs;
    bool holds;
  };

  // Its states, compared with their values at the last run, which `record` takes as they differ and `forget` drops,
  // and the run, made
  // for their values now: erased, as their types are not the effect's.
  // What `record` saw, none for `forget`.
  std::function<std::optional<watched>(watching, void const *)> watch_;
  std::function<async::task<>(std::stop_token, void const *)> run_;
  // The type of the props it reads, none for an effect on states.
  async::detail::type_id props_type_{};
  detail::run_slot slot_;
  detail::target_hooks target_;
  detail::state_link link_{};
  // Failed to start, until reload().
  bool failed_ = false;
};

static_assert(detail::LinkedMember<effect>);
static_assert(detail::EffectMember<effect>);

} // namespace quoll::ui

#endif // QUOLL_UI_EFFECT_HPP
