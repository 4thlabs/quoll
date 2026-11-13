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

#ifndef QUOLL_UI_CONTEXT_PROVIDED_HPP
#define QUOLL_UI_CONTEXT_PROVIDED_HPP

#include <quoll/ui/context/provide.hpp>
#include <quoll/ui/detail/context/context_entry.hpp>
#include <quoll/ui/detail/context/context_member.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/state_link.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace quoll::ui {

/**
 * @brief A context a component provides to the components of its view, and theirs, as React's Provider: found by
 * `use_context<T>()` among them before the app's, and gone with the component.
 *
 * @code
 * inline constexpr component<> checkout_page = [] {
 *   // The cart being filled, for the steps of the page: gone as the page goes.
 *   auto &cart = provide_context<cart_draft>();
 *
 *   return div({cart_lines(), payment_step()});
 * };
 *
 * inline constexpr component<> cart_lines = [] {
 *   auto &cart = use_context<cart_draft>();
 *   // ...
 * };
 * @endcode
 *
 * What provide_context() gives a component. T is a context, as provide<T>() takes: its states show again the components
 * reading it, and the component providing it; its effects start after each view of that component, as its tasks; it
 * uses the contexts its component finds. A component using it keeps the component providing it while it lives, and
 * run() as long as its work.
 *
 * Neither copied nor moved: the components using it refer to it. Thread safety: none, as a state.
 */
template <class T>
class provided {
  static_assert(
      std::is_aggregate_v<T> && !std::is_const_v<T>,
      "quoll::ui: a context is an aggregate: public members and no constructor"
  );

public:
  using provided_type = T;

  provided() : entry_(std::make_unique<detail::context_entry>(detail::context_table_of<T>, detail::state_link{})) {
    static_cast<void>(provide<T>());
  }

  provided(provided const &) = delete;
  provided(provided &&) = delete;
  provided &operator=(provided const &) = delete;
  provided &operator=(provided &&) = delete;

  ~provided() = default;

  /** @brief The context. */
  [[nodiscard]] T &operator*() const noexcept { return *get(); }

  /** @brief See operator*(). */
  [[nodiscard]] T *operator->() const noexcept { return get(); }

  /** @brief For the app of its component, as a state's: its changes are told to the component. */
  void link(detail::state_link to) noexcept { entry_->link(to); }

  /** @brief For the app, after each view of its component: the effects of the context whose states changed start. */
  void after_view(detail::effect_starter const &start) {
    // Its context said, as it was made, what its effects' targets are before their first view.
    if (start.first) {
      return;
    }

    if (auto const effects = entry_->table().effects; effects != nullptr) {
      effects(entry_->object(), start);
    }
  }

  /** @brief For the app making its component: the context finds those it uses, as its component's would. */
  void bind(detail::context_finder const &finder) {
    if (auto const bind = entry_->table().bind; bind != nullptr) {
      bind(entry_->object(), finder);
    }
  }

  /** @brief For the components using it. */
  [[nodiscard]] detail::context_entry &entry() noexcept { return *entry_; }

private:
  friend struct detail::context_access;

  // What its component's view sees of it, and of the contexts it uses, kept once that view shows, as a shared<T>'s:
  // a change of a context it uses shows its component again, which reads it through it.
  void mark() noexcept { marked_ = entry_->version(); }

  void commit() noexcept { seen_ = marked_; }

  [[nodiscard]] bool changed() const noexcept { return entry_->version() != seen_; }

  [[nodiscard]] T *get() const noexcept { return static_cast<T *>(entry_->object()); }

  std::unique_ptr<detail::context_entry> entry_;
  std::uint64_t marked_ = 0;
  std::uint64_t seen_ = 0;
};

} // namespace quoll::ui

#endif // QUOLL_UI_CONTEXT_PROVIDED_HPP
