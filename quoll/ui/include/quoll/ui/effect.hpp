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
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/effect_member.hpp>
#include <quoll/ui/detail/effect_starter.hpp>
#include <quoll/ui/detail/linked_member.hpp>
#include <quoll/ui/detail/run_slot.hpp>
#include <quoll/ui/detail/state_link.hpp>
#include <quoll/ui/run_error.hpp>
#include <quoll/ui/run_status.hpp>
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

// Runs `work` as its component's task, its result kept by the slot it runs for, unless the component stopped since it
// was started, as one that went with the app before its executor ran: an effect does nothing once unmounted() ran.
template <class T>
async::task<> started(async::task<T> work) {
  auto const stop = co_await async::current_stop_token();

  if (stop.stop_requested()) {
    co_return;
  }

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
 *     return div({when(sales.status() == run_status::done, shown)});
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
 * the component ends a run with it, as React's cleanup. reload() runs it again for the value the state holds now, after
 * the view it asks for, as a change of the state would: one way to run it, as React has.
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
    static_assert(!detail::is_view<std::remove_cvref_t<D>> &&
                      !detail::is_view<std::remove_cvref_t<typename detail::dependency_traits<D>::value_type>>,
                  "quoll::ui::effect: its state's value is copied into the run, which a view would outlive");

    using traits = detail::dependency_traits<D>;
    using dependency_value = typename traits::value_type;
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
      return maker{[function, value = dependency_value{traits::value(*dependency)}]() mutable {
        return function(std::move(value));
      }};
    };
  }

  effect(effect const &) = delete;
  effect(effect &&) = delete;
  effect &operator=(effect const &) = delete;
  effect &operator=(effect &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~effect() { assert(slot_.frames == 0 && "quoll::ui::effect: destroyed while a run is under way"); }

  /**
   * @brief Runs it again for the value its state holds now, as a change of the state would: after the view it asks
   * for, the run before stopped, as React runs an effect again. Nothing runs while the state holds an empty optional.
   * What asking for that view throws goes to the caller, as a state's change does.
   */
  void reload() {
    watch_(watching::forget);
    detail::tell(link_);
  }

  /**
   * @brief Where it is: not run, running, or done. Its state changed since its last run, or reload() was asked for, as
   * in the view that makes, which comes before the run, it is running, or none once the state holds none: that view
   * shows no result of the value before.
   */
  [[nodiscard]] run_status status() const {
    if (auto const now = watch_(watching::peek); now.differs) {
      return now.holds ? run_status::running : run_status::none;
    }

    return detail::status_of(slot_.pending, slot_.result.has_value());
  }

  /**
   * @brief What the last run gave, or what it threw.
   * @throws std::logic_error unless status() is done.
   */
  [[nodiscard]] result_type const &result() const QUOLL_LIFETIMEBOUND {
    return detail::result_of(status() == run_status::done ? &slot_ : nullptr, "quoll::ui::effect: no run is done");
  }

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

  /** @brief For the app of the component holding it, after each of its views: runs it if its state changed. */
  void after_view(detail::effect_starter const &start) {
    if (!watch_(watching::peek).differs) {
      return;
    }

    auto next = current_();

    watch_(watching::record);

    if (!next) {
      idle();
      return;
    }

    // Asked for now, as the view showed it running: the run before stops at once, so that an answer of its that comes
    // before this one starts writes nothing, and what it gave is gone.
    if (slot_.pending) {
      slot_.last.request_stop();
    }

    ++slot_.current;
    slot_.pending = true;
    slot_.result.reset();

    // One that cannot start, as for a lack of memory, ends on that failure, which the view shows and reload() tries
    // again, rather than be tried after each view; it is told as the app's failures are.
    try {
      start(detail::started(detail::run_in<T>(slot_, link_, std::move(*next), false, true, false)));
    } catch (...) {
      slot_.pending = false;
      slot_.result.emplace(std::unexpect, std::current_exception());
      detail::tell_quietly(link_);
      throw;
    }
  }

private:
  // Its state holds none: the run under way, if any, is stopped and writes nothing, and nothing is left, as the view
  // that made it showed.
  void idle() {
    if (slot_.pending) {
      slot_.last.request_stop();
    }

    ++slot_.current;
    slot_.pending = false;
    slot_.result.reset();
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
