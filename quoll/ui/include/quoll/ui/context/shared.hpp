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

#ifndef QUOLL_UI_CONTEXT_SHARED_HPP
#define QUOLL_UI_CONTEXT_SHARED_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/component/call.hpp>
#include <quoll/ui/detail/context/bind_contexts.hpp>
#include <quoll/ui/detail/context/context_entry.hpp>
#include <quoll/ui/detail/context/context_member.hpp>
#include <quoll/ui/detail/context/member_join.hpp>
#include <quoll/ui/detail/data/effect_function.hpp>
#include <quoll/ui/detail/view/view_context.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/**
 * @brief The context of type T of the app holding a component: data its components share, as React's contexts.
 *
 * @code
 * struct catalog {
 *   async_state<std::vector<products::product>> items;
 *   effect load{std::tie(), products_client::list, items};
 *
 *   task<products::product> save(std::int64_t id, products::product_update changes);
 * };
 *
 * inline constexpr component<> product_list = [] {
 *   auto &products = use_context<catalog>();
 *
 *   if (products->items->loading) {
 *     return p("Loading the products");
 *   }
 *   // ...
 * };
 * @endcode
 *
 * What use_context() gives a component, or a member of a context using another, found by its type among the
 * contexts the components above provide, nearest first, then those its app's options give (see provide()). A change of
 * the context's states shows again the components using it, as theirs would; its effects start after each view of the
 * app. run() runs a member function of the context as a task of the app, not the caller's: a component that goes stops
 * waiting for it, and the work goes on, so that what it changes shows wherever it is read, as a callback's does.
 *
 * The app holds its context as long as it lives, past the tasks of its components. Thread safety: none, as its app.
 */
template <class T>
class shared {
  static_assert(
      !std::is_const_v<T> && !std::is_volatile_v<T>,
      "quoll::ui::shared: a context is found by its type, as provide<T>() gave it, with no const"
  );

public:
  using element_type = T;

  shared() = default;

  // Neither copied nor moved: it lives in the component the app made, which the app outlives, and a copy kept elsewhere
  // could outlive the app and its context.
  shared(shared const &) = delete;
  shared(shared &&) = delete;
  shared &operator=(shared const &) = delete;
  shared &operator=(shared &&) = delete;

  ~shared() = default;

  /**
   * @brief The context.
   * @throws std::logic_error for a component no app made, as one a view written once (to_html) shows.
   */
  [[nodiscard]] T &operator*() const { return *get(); }

  /** @brief See operator*(). */
  [[nodiscard]] T *operator->() const { return get(); }

  /**
   * @brief Runs `member` of the context with copies of `args`, as a task of the app, and waits for its end: the caller
   * that stops stops waiting, and the work goes on. What it throws reaches the caller, or, once it stopped waiting, the
   * app's on_error.
   */
  template <class Member, class... A>
    requires std::invocable<Member &, T &, std::decay_t<A> &&...> &&
             requires { typename detail::task_value<std::invoke_result_t<Member &, T &, std::decay_t<A> &&...>>::type; }
  [[nodiscard]] auto run(Member member, A &&...args) const {
    using R = typename detail::task_value<std::invoke_result_t<Member &, T &, std::decay_t<A> &&...>>::type;
    using function_type = std::function<async::task<R>(std::decay_t<A> && ...)>;

    static_assert(
        (!detail::holds_view<std::decay_t<A>> && ...) && !detail::holds_view<R>,
        "quoll::ui::shared: what run() gives the context, and gives back, outlives the caller: none can be "
        "a view"
    );

    // Not a read: a component that only runs on its context does not show again as it changes.
    auto *const context = static_cast<T *>(entry().object());

    // Mutable, so that `member` is called as the constraint says.
    // Holds what provides the context, a component above, until the work ends: its caller may go first.
    auto function =
        std::make_shared<function_type const>([context, member, keeper = keeper_](std::decay_t<A> &&...values) mutable {
          return std::invoke(member, *context, std::move(values)...);
        });

    return detail::call_in<R>(
        std::move(function), detail::call_target{.app = app_}, std::decay_t<A>(std::forward<A>(args))...
    );
  }

  /**
   * @brief For the app making the component holding it: finds its context, none for a view written once.
   * @throws std::logic_error for an app given no context of type T.
   */
  void bind(detail::context_finder const &finder) {
    auto found = finder(async::detail::type_id_of<T>());

    // A view written once finds none, and stays unbound: using it then throws.
    if (found.entry == nullptr && finder.find == nullptr) {
      return;
    }

    if (found.entry == nullptr) {
      throw std::logic_error{detail::missing_context};
    }

    entry_ = found.entry;
    keeper_ = std::move(found.keeper);
    app_ = finder.app;
  }

  /** @brief How many times the states of its context, and of those it uses, changed. */
  [[nodiscard]] std::uint64_t context_version() const noexcept { return entry_ == nullptr ? 0 : entry_->version(); }

private:
  friend struct detail::context_access;

  // As its component's view is about to run: what it sees of its context, before the view may change it, kept once the
  // view shows, so that a view that throws leaves what it saw before.
  void mark() noexcept { marked_ = context_version(); }

  void commit() noexcept { seen_ = marked_; }

  // Whether its context changed since its component's last view that showed, once read through it: a component that
  // only runs on its context does not show again as it changes.
  [[nodiscard]] bool changed() const noexcept { return read_ && context_version() != seen_; }

  [[nodiscard]] detail::context_entry &entry() const {
    if (entry_ == nullptr) {
      throw std::logic_error{
          "quoll::ui::shared: used by a component no app made, or held other than as a member of the "
          "component itself, as a state is"
      };
    }

    return *entry_;
  }

  // A read: from then on, the context's changes show the component again.
  [[nodiscard]] T *get() const {
    auto *const context = static_cast<T *>(entry().object());

    read_ = true;
    return context;
  }

  detail::context_entry *entry_ = nullptr;
  mutable bool read_ = false;
  std::uint64_t marked_ = 0;
  std::uint64_t seen_ = 0;
  std::shared_ptr<detail::app_link> app_{};
  // What provides its context, a component above, kept as long as this one: none for the app's.
  std::shared_ptr<void const> keeper_{};
  // Last: joins the component being made once it is made, if it is one of its members.
  detail::member_join<shared> joined_{*this};
};

namespace detail {

struct context_model {};

} // namespace detail

static_assert(detail::ContextMember<shared<detail::context_model>>);

namespace detail {

// What a shared<T> member of a component written as a struct gives it: what its view saw, read or not.
template <class T>
struct joined_context<shared<T>> {
  static void mark(void *member) noexcept { context_access::mark(*static_cast<shared<T> *>(member)); }

  static void commit(void *member) noexcept { context_access::commit(*static_cast<shared<T> *>(member)); }

  static bool changed(void const *member) noexcept {
    return context_access::changed(*static_cast<shared<T> const *>(member));
  }
};

} // namespace detail

} // namespace quoll::ui

#endif // QUOLL_UI_CONTEXT_SHARED_HPP
