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

#ifndef QUOLL_UI_DATA_RESOURCE_HPP
#define QUOLL_UI_DATA_RESOURCE_HPP

#include <quoll/ui/data/async_state.hpp>
#include <quoll/ui/data/effect.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/data/effect_function.hpp>
#include <quoll/ui/detail/data/effect_member.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/loading_view.hpp>
#include <quoll/ui/detail/data/props_follow.hpp>
#include <quoll/ui/detail/data/resource_function.hpp>
#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/view/element.hpp>

#include <concepts>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief A load: a state and the effect loading it, read as one value, `match(loading, failed, loaded)`, as Solid's
 * createResource and Dioxus's use_resource.
 *
 * @code
 * struct product_view : component<product_view> {
 *   struct props {
 *     products::product item;
 *
 *     bool operator==(props const &) const = default;
 *   };
 *
 *   resource<products::monthly_sales> sales{[](props const &given) { return given.item.id; }, products_client::sales};
 *
 *   element view() {
 *     return sales.match(
 *         [] { return p("Loading its sales"); },
 *         [](std::exception_ptr const &error) { return sales_failed(error); },
 *         [](products::monthly_sales const &loaded) { return sales_chart({.sales = loaded}); }
 *     );
 *   }
 * };
 * @endcode
 *
 * A member of a component written as a struct, its effect following what its props give, as an effect's does: a field
 * of them, `&props::id`, or a function of them; or of a struct or a context, following its states, `filter` or
 * `std::tie(filter, page)`.
 * Its function returns a `task<T>`: it runs as its component is first shown, and again as what it follows changes, the
 * run before stopped, or as reload() asks; a failure reaches the app's on_error too, as an effect's.
 *
 * `match` gives what to show: the first function while a run is under way, given nothing or what the last run that
 * ended well gave, `std::optional<T> const &`, none before the first, for a view that keeps it while it loads again;
 * the second once the last run failed, given why; the third once it gave a value. With four, the first shows it
 * before any run has started, and once what it follows is an optional holding none, which drops what was loaded; with
 * three, the loading one does.
 * Neither copied nor moved, as an effect. Thread safety: none, as a state.
 */
template <class T>
class resource {
public:
  static_assert(std::is_object_v<T>, "quoll::ui::resource: a load gives a value, a task<T> of an object");
  static_assert(
      !detail::holds_view<T>, "quoll::ui::resource: what it loads is kept, as a value: a view would outlive its run"
  );

  using value_type = T;

  /**
   * @brief Loads with `function` what `follow`, a field of its component's props or a function of them, gives, as it
   * changes: see effect.
   * @throws std::logic_error outside the making of a struct, or for the props of another.
   * @throws std::invalid_argument for no field or no function, as a null pointer.
   */
  template <class Follow, class F>
    requires detail::PropsFollow<Follow> && detail::EffectFunction<F, typename detail::follow_traits<Follow>::value>
  resource(Follow follow, F function, effect_options options = {})
      : load_{
            std::move(follow), wrapped<F, typename detail::follow_traits<Follow>::value>(std::move(function)), loaded_,
            options
        } {}

  /**
   * @brief Loads with `function` what `dependencies`, from std::tie, hold, as one changes: see effect.
   * @throws std::invalid_argument for no function, as a null pointer.
   */
  template <class... D, class F>
    requires detail::EffectFunction<F, D...>
  resource(std::tuple<state<D> &...> dependencies, F function, effect_options options = {})
      : load_{dependencies, wrapped<F, D...>(std::move(function)), loaded_, options} {}

  /**
   * @brief Loads with `function` what `dependency` holds, as it changes: see effect.
   * @throws std::invalid_argument for no function, as a null pointer.
   */
  template <class D, class F>
    requires detail::EffectFunction<F, D>
  resource(state<D> &dependency, F function, effect_options options = {})
      : resource(std::tie(dependency), std::move(function), options) {}

  resource(resource const &) = delete;
  resource(resource &&) = delete;
  resource &operator=(resource const &) = delete;
  resource &operator=(resource &&) = delete;
  ~resource() = default;

  /** @brief Whether a run is under way, for what needs only the flag, as a button's label. */
  [[nodiscard]] bool loading() const noexcept { return loaded_->loading; }

  /** @brief Loads again, the run before stopped: see effect::reload(). */
  void reload() { load_.reload(); }

  /** @brief For the app, or the component of its hook, as a state's: those of a context, which finds it so. */
  void link(detail::state_link to) noexcept {
    loaded_.link(to);
    load_.link(to);
  }

  /** @brief For the app, or the component of its hook, after each view: see effect::after_view(). */
  void after_view(detail::effect_starter const &start) { load_.after_view(start); }

  /** @brief What to show: while it loads, once it failed, once it gave a value. See resource. */
  template <class Loading, class Failed, class Loaded>
    requires detail::LoadingView<Loading, T> &&
             std::is_invocable_r_v<element, Failed const &, std::exception_ptr const &> &&
             std::is_invocable_r_v<element, Loaded const &, T const &>
  [[nodiscard]] element match(Loading const &loading, Failed const &failed, Loaded const &loaded) const {
    auto const &now = *loaded_;

    if (now.loading || (!now.error && !now.value.loaded)) {
      return show_loading(loading, now.value.loaded);
    }

    if (now.error) {
      return failed(now.error);
    }

    return loaded(*now.value.loaded);
  }

  /** @brief What to show, and before any run has started, `idle`. See resource. */
  template <class Idle, class Loading, class Failed, class Loaded>
    requires std::is_invocable_r_v<element, Idle const &> && detail::LoadingView<Loading, T> &&
             std::is_invocable_r_v<element, Failed const &, std::exception_ptr const &> &&
             std::is_invocable_r_v<element, Loaded const &, T const &>
  [[nodiscard]] element
  match(Idle const &idle, Loading const &loading, Failed const &failed, Loaded const &loaded) const {
    auto const &now = *loaded_;

    if (!now.loading && !now.error && !now.value.loaded) {
      return idle();
    }

    return match(loading, failed, loaded);
  }

private:
  // Its function, each run's result held: null when it is, which its effect refuses.
  template <class F, class... D>
  [[nodiscard]] static detail::resource_function<T, F> wrapped(F function) {
    static_assert(
        std::same_as<detail::effect_value_t<F, D...>, T>, "quoll::ui::resource: the function of a resource<T> gives T"
    );

    return {.function = std::move(function)};
  }

  template <class Loading>
  [[nodiscard]] static element show_loading(Loading const &loading, std::optional<T> const &before) {
    if constexpr (std::is_invocable_r_v<element, Loading const &>) {
      return loading();
    } else {
      return loading(before);
    }
  }

  // Its state before its effect, which writes into it.
  async_state<detail::load_result<T>> loaded_;
  effect load_;
};

static_assert(detail::LinkedMember<resource<int>>);
static_assert(detail::EffectMember<resource<int>>);

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_RESOURCE_HPP
