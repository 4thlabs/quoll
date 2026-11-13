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

#ifndef QUOLL_UI_ROUTING_ROUTER_HPP
#define QUOLL_UI_ROUTING_ROUTER_HPP

#include <quoll/ui/component/component.hpp>
#include <quoll/ui/context/context_spec.hpp>
#include <quoll/ui/context/provide.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/component/held_component.hpp>
#include <quoll/ui/detail/routing/route_table.hpp>
#include <quoll/ui/detail/view/hot.hpp>
#include <quoll/ui/routing/history.hpp>
#include <quoll/ui/view/element.hpp>
#include <quoll/ui/view/tags.hpp>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::ui {

/** @brief An address of an app: its path, percent-encoded as in a link, and its query, after `?`, none without one. */
struct location {
  std::string path{"/"};
  std::string query{};

  [[nodiscard]] bool operator==(location const &) const = default;
};

namespace detail {

// What a location matched: the routes from the top to the one of the page, and the values of the address.
struct route_match {
  std::vector<std::size_t> chain;
  route_values values;
};

// The routing of an app, but for its state: its routes, its history, the match of the location now.
class router_core {
public:
  router_core() = default;

  router_core(router_core const &) = delete;
  router_core(router_core &&) = delete;
  router_core &operator=(router_core const &) = delete;
  router_core &operator=(router_core &&) = delete;

  ~router_core();

  // Routes `routes` through `history`, owning `owned` when it is a memory history of its own, its location in `where`.
  void start(
      std::span<route_entry const> routes, history_ref history, std::unique_ptr<memory_history> owned,
      state<location> &where
  );

  // Goes to `href`, pushed onto the history or replacing its last, a redirect followed.
  void go(std::string_view href, bool replace);

  void back() const;

  // The page at `depth` under the routes the location matched, none past them.
  [[nodiscard]] element show(std::size_t depth);

  // The address of the page of the table `page`, a component a route names, for `props`, by its first route.
  [[nodiscard]] std::string href(component_table const &page, void const *props) const;

private:
  static void changed(void *self, std::string_view href);

  // Sets the location to `href`, the history gave, following redirects, which replace it in the history.
  void arrive(std::string_view href);

  // Where `href` ends, its redirects followed, in the one form an address has.
  [[nodiscard]] location resolved(std::string_view href) const;

  // Sets the location, which shows it again unless it is the same.
  void set_location(location where);

  std::span<route_entry const> routes_{};
  std::unique_ptr<memory_history> owned_{};
  std::optional<history_ref> history_{};
  state<location> *where_ = nullptr;
  // The match of the location, as long as it stays: none before the first.
  std::optional<location> matched_at_{};
  route_match matched_{};
};

} // namespace detail

/**
 * @brief The routing of an app, a context its components find through `use_context<router>()`: the location, the
 * address shown, and what goes elsewhere.
 *
 * @code
 * inline constexpr component<> buy_button = [] {
 *   auto &nav = use_context<ui::router>();
 *   auto const open = [&nav] { nav->go(product_page, {.id = 7}); };
 *
 *   return button("Open") | on_click(open);
 * };
 * @endcode
 *
 * Made by routing() in an app's options. Going elsewhere pushes the address onto the history, but for the address
 * shown, or one that redirects to it, which takes its place, as a browser's link to the page it shows does; its back
 * button goes back through; a component reading `where` shows again as it changes.
 */
struct router {
  /** @brief The location now. */
  state<location> where;
  /** @brief For Quoll. */
  detail::router_core core;

  /** @brief Goes to `href`, an address of the app, as a link to it does: pushed, unless it is the one shown. */
  void go(std::string_view href) { core.go(href, false); }

  /** @brief Goes to the page `page`, the component a route names, for `props`, by its first route. */
  template <class P>
  void go(component_base<P> const &page, std::type_identity_t<P> const &props) {
    core.go(href(page, props), false);
  }

  /** @brief Goes to the page `page`, without props. */
  void go(component_base<void> const &page) { core.go(href(page), false); }

  /** @brief Goes to `href` in place of the address now, which back() then does not come back to. */
  void replace(std::string_view href) { core.go(href, true); }

  /** @brief Goes back, as the browser's button does. */
  void back() const { core.back(); }

  /**
   * @brief The address of the page `page`, the component a route names, for `props`, by its first route: the fields
   * of `props` that route names, a field it does not name in no address.
   * @throws std::logic_error when no route shows it.
   */
  template <class P>
  [[nodiscard]] std::string href(component_base<P> const &page, std::type_identity_t<P> const &props) const {
    return core.href(page.table(), &props);
  }

  /** @brief The address of the page `page`, without props. */
  [[nodiscard]] std::string href(component_base<void> const &page) const { return core.href(page.table(), nullptr); }
};

/**
 * @brief The routing of the routes `Routes` for an app's options, a context of type router, through a history in
 * memory starting at `start`, as a program with no browser and a test have.
 *
 * @code
 * mount(app_shell, "#app", {.contexts = {ui::routing<routes>(quoll::emscripten::browser_history())}});
 * @endcode
 */
template <auto const &Routes>
[[nodiscard]] context_spec routing(std::string start = "/") {
  auto made = [start = std::move(start)] {
    auto held = detail::make_held<router>();
    auto &made_router = *static_cast<router *>(held.get());
    auto history = std::make_unique<memory_history>(start);
    auto const ref = history->ref();

    made_router.core.start(Routes.entries, ref, std::move(history), made_router.where);
    return held;
  };

  return detail::context_spec_access::made_with(detail::context_table_of<router>, std::move(made));
}

/** @brief The routing of `Routes` through `history`, the browser's, which outlives the app. */
template <auto const &Routes>
[[nodiscard]] context_spec routing(history_ref history) {
  auto made = [history] {
    auto held = detail::make_held<router>();
    auto &made_router = *static_cast<router *>(held.get());

    made_router.core.start(Routes.entries, history, nullptr, made_router.where);
    return held;
  };

  return detail::context_spec_access::made_with(detail::context_table_of<router>, std::move(made));
}

namespace detail {

// What outlets above give the one below: its depth among the routes of the location.
struct outlet_level {
  std::size_t depth = 0;
};

// The depth of an outlet in the view running now: one more than the nearest above, none for the first.
[[nodiscard]] std::size_t outlet_depth();

// The router of the app of the view running now: throws std::logic_error for an app given no routing.
[[nodiscard]] router &router_in_view();

// A link to `href`: an `a` its click leaves on the page, going there through `nav`, as a browser does without it.
[[nodiscard]] element linked(router &nav, std::string href, element content);

} // namespace detail

/**
 * @brief Where the page of the location shows, in the view of the app's own component or of a layout: the page of
 * the routes it matched, at the depth of the outlet, the layouts above it outlets of their own.
 */
[[nodiscard]] element outlet();

/** @brief A link to `href`, an address of the app: a click goes there without leaving the page, as go() does. */
[[nodiscard]] element link_to(std::string_view href, std::initializer_list<detail::child> children);

/**
 * @brief See link_to(). Inline, as a text's type is the program's, which a hot reload changes (`QUOLL_UI_HOT`), and
 * not the library's.
 */
[[nodiscard]] inline element link_to(std::string_view href, detail::literal_text text) {
  return link_to(href, {ui::text(text)});
}

/**
 * @brief A link to the page `page`, the component a route names, for `props`, by its first route, its props checked at
 * compile time: the fields of `props` that route names, as href() writes them.
 * @throws std::logic_error when no route shows it.
 */
template <class P>
[[nodiscard]] element
link_to(component_base<P> const &page, std::type_identity_t<P> const &props, detail::literal_text text) {
  auto &nav = detail::router_in_view();

  return detail::linked(nav, nav.href(page, props), node("a", {ui::text(text)}));
}

/** @brief See link_to(). */
template <class P>
[[nodiscard]] element link_to(
    component_base<P> const &page, std::type_identity_t<P> const &props, std::initializer_list<detail::child> children
) {
  auto &nav = detail::router_in_view();

  return detail::linked(nav, nav.href(page, props), node("a", children));
}

/** @brief A link to the page `page`, without props. */
[[nodiscard]] element link_to(component_base<void> const &page, std::initializer_list<detail::child> children);

/** @brief See link_to(), inline for its text, as the one to an address. */
[[nodiscard]] inline element link_to(component_base<void> const &page, detail::literal_text text) {
  return link_to(page, {ui::text(text)});
}

} // namespace quoll::ui

#endif // QUOLL_UI_ROUTING_ROUTER_HPP
