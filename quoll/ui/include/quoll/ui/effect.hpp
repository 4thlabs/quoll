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

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/effect_member.hpp>
#include <quoll/ui/detail/effect_starter.hpp>
#include <quoll/ui/detail/linked_member.hpp>
#include <quoll/ui/detail/run_slot.hpp>
#include <quoll/ui/detail/state_link.hpp>
#include <quoll/ui/run_error.hpp>
#include <quoll/ui/state.hpp>

#include <cassert>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief Where an effect is. */
enum class effect_status {
  /** @brief It has not run: its dependency holds none yet, as an empty optional. */
  none,
  /** @brief A run is under way. */
  running,
  /** @brief The last run ended, with a value or a failure: its result(). */
  done,
  /** @brief The last run was stopped by its caller, as a timeout does, before it ended. */
  stopped,
};

namespace detail {

template <class D>
struct dependency_traits {
  using value_type = D;

  [[nodiscard]] static bool holds(D const &) noexcept { return true; }

  [[nodiscard]] static D const &value(D const &dependency) noexcept { return dependency; }
};

// An optional dependency runs the effect with what it holds, and not at all while it holds none.
template <class X>
struct dependency_traits<std::optional<X>> {
  using value_type = X;

  [[nodiscard]] static bool holds(std::optional<X> const &dependency) noexcept { return dependency.has_value(); }

  [[nodiscard]] static X const &value(std::optional<X> const &dependency) noexcept { return *dependency; }
};

// Starts nothing more than `work`, its result kept by the slot it runs for.
template <class T>
async::task<> ignored(async::task<T> work) {
  static_cast<void>(co_await std::move(work));
}

} // namespace detail

/**
 * @brief What a component runs as a state of its changes, as React's useEffect with that state as its dependency: after
 * each of its views in which the state changed, the run before stopped, and its result, a value or a failure, kept.
 *
 * @code
 * struct dashboard_page {
 *   state<std::optional<std::int64_t>> selected;
 *
 *   effect<products::monthly_sales> sales{selected, [](std::int64_t id) {
 *     return products_client::sales(id);
 *   }};
 *
 *   element view() {
 *     auto const shown = [this] {
 *       auto const &result = sales.result();
 *
 *       return result ? component<sales_chart>({.sales = *result}) : p("No sales");
 *     };
 *
 *     return div({when(sales.status() == effect_status::done, shown)});
 *   }
 * };
 * @endcode
 *
 * A member of a component, which shows again as it changes, as a state does, and which depends on a state of the same
 * component, declared before it. After a view of the component, once it is shown, as React runs its effects once a
 * render is committed, the effect compares the state with its value at the last run, with `==`: changed, the run
 * before is stopped, so that its answer writes nothing, even one that ignored the stop and came anyway, and the
 * function runs, as a task of the component, with a copy of the value. A state holding a `std::optional` runs it with
 * what the optional holds, and stops it, with nothing run, once the optional is empty. The function returns a task of
 * T, or of nothing for `effect<>`; what it throws, before giving its task too, is the failure of its result. A stop of
 * the component ends a run with it, as React's cleanup. reload() runs it again for the value the state holds now.
 *
 * It keeps a copy of the result: a value whose copy throws is the run's failure, given and kept.
 *
 * Neither copied nor moved: a run under way refers to it, and a component stays where it was made. Its value and the
 * state's are values: a reference or a view would outlive what it refers to. Thread safety: none, as a state.
 */
template <class T = void>
class effect {
  static_assert(!std::is_reference_v<T> && !detail::is_view<std::remove_cvref_t<T>>,
                "quoll::ui::effect: its value is kept, as a value");
  static_assert(std::is_void_v<T> || std::copyable<T>,
                "quoll::ui::effect: its value is copied, into what it holds and what it gives");

public:
  using value_type = T;
  using result_type = std::expected<T, std::exception_ptr>;

  /**
   * @brief Runs `function` with what `dependency`, a state of the same component, holds, as it changes.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class D, class F>
    requires std::invocable<F &, typename detail::dependency_traits<D>::value_type &&> &&
             std::same_as<std::invoke_result_t<F &, typename detail::dependency_traits<D>::value_type &&>,
                          async::task<T>> &&
             std::equality_comparable<D> && std::copyable<D> && std::copy_constructible<F>
  effect(state<D> &dependency, F function) {
    static_assert(!detail::is_view<std::remove_cvref_t<D>>,
                  "quoll::ui::effect: its state's value is copied into the run, which a view would outlive");

    using traits = detail::dependency_traits<D>;
    using value_type = typename traits::value_type;
    using maker = std::function<async::task<T>()>;

    if constexpr (requires { function == nullptr; }) {
      if (function == nullptr) {
        throw std::invalid_argument{"quoll::ui::effect: no function"};
      }
    }

    watch_ = [&dependency, last = std::optional<D>{}](watching asked) mutable {
      bool const differs = !last || !(*last == *dependency);

      if (asked == watching::record) {
        last = *dependency;
      } else if (asked == watching::forget) {
        last.reset();
      }

      return watched{.differs = differs, .holds = traits::holds(*dependency)};
    };

    current_ = [&dependency, function = std::move(function)]() -> std::optional<maker> {
      if (!traits::holds(*dependency)) {
        return std::nullopt;
      }

      // Its own copy of the value, given as an rvalue: one taking it by reference refers to the run's copy.
      return maker{
          [function, value = value_type{traits::value(*dependency)}]() mutable { return function(std::move(value)); }};
    };
  }

  effect(effect const &) = delete;
  effect(effect &&) = delete;
  effect &operator=(effect const &) = delete;
  effect &operator=(effect &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~effect() { assert(slot_.frames == 0 && "quoll::ui::effect: destroyed while a run is under way"); }

  /**
   * @brief Runs it again for the value its state holds now, stopping the run before: what it gave, or what it threw,
   * as result() then holds; run_replaced once a later run took its place.
   * @throws std::logic_error while its state holds an empty optional: there is nothing to run for.
   */
  [[nodiscard]] async::task<result_type> reload() QUOLL_LIFETIMEBOUND {
    auto next = current_();

    if (!next) {
      throw std::logic_error{"quoll::ui::effect: nothing to run for, its state holding none"};
    }

    // Run for the value the state holds now: the view after a change does not run it a second time.
    watch_(watching::record);
    return detail::run_in<T>(slot_, link_, std::move(*next), false, true);
  }

  /**
   * @brief Where it is: not run, running, done, or stopped by its caller. Its state changed since its last run, as in
   * the view the change makes, which comes before the run, it is running, or none once the state holds none: that view
   * shows no result of the value before.
   */
  [[nodiscard]] effect_status status() const {
    if (auto const now = watch_(watching::peek); now.differs) {
      return now.holds ? effect_status::running : effect_status::none;
    }

    if (slot_.pending) {
      return effect_status::running;
    }

    if (slot_.stopped) {
      return effect_status::stopped;
    }

    return slot_.result ? effect_status::done : effect_status::none;
  }

  /**
   * @brief What the last run gave, or what it threw.
   * @throws std::logic_error unless status() is done.
   */
  [[nodiscard]] result_type const &result() const QUOLL_LIFETIMEBOUND {
    return detail::result_of(status() == effect_status::done ? &slot_ : nullptr, "quoll::ui::effect: no run is done");
  }

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

  /** @brief For the app of the component holding it, after each of its views: runs it if its state changed. */
  void after_view(detail::effect_starter const &start) {
    if (!watch_(watching::peek).differs) {
      return;
    }

    // Recorded once it starts: one that cannot, as for a lack of memory, is tried again after the next view.
    try {
      auto next = current_();

      if (!next) {
        watch_(watching::record);
        idle();
        return;
      }

      auto work = detail::ignored(detail::run_in<T>(slot_, link_, std::move(*next), false, true));

      watch_(watching::record);
      start(std::move(work));
    } catch (...) {
      watch_(watching::forget);
      throw;
    }
  }

private:
  // Its state holds none: the run under way, if any, is stopped and writes nothing, and nothing is left.
  void idle() {
    bool const had = slot_.pending || slot_.stopped || slot_.result.has_value();

    if (slot_.pending) {
      slot_.last.request_stop();
    }

    ++slot_.current;
    slot_.pending = false;
    slot_.stopped = false;
    slot_.result.reset();

    // As it first shows, with nothing yet, it says nothing new.
    if (had) {
      detail::tell_quietly(link_);
    }
  }

  enum class watching { peek, record, forget };

  struct watched {
    bool differs;
    bool holds;
  };

  // The state, compared with its value at the last run, which `record` takes and `forget` drops, and the function, made
  // for its value now: erased, as their types are not the effect's. Mutable, as a peek changes nothing.
  mutable std::function<watched(watching)> watch_;
  std::function<std::optional<std::function<async::task<T>()>>()> current_;
  detail::run_slot<T> slot_;
  detail::state_link link_{};
};

static_assert(detail::LinkedMember<effect<std::string>>);
static_assert(detail::EffectMember<effect<std::string>>);
static_assert(detail::LinkedMember<effect<>>);
static_assert(detail::EffectMember<effect<>>);

} // namespace quoll::ui

#endif // QUOLL_UI_EFFECT_HPP
