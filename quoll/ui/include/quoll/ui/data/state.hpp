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

#ifndef QUOLL_UI_DATA_STATE_HPP
#define QUOLL_UI_DATA_STATE_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/state_link.hpp>

#include <concepts>
#include <utility>

namespace quoll::ui {

/**
 * @brief A value of a component that the interface depends on: changing it shows again, once, what the component views.
 *
 * A member of a component, linked to its app when it is mounted. Changed through set(), `=` or update(), since a change
 * through a reference would go unnoticed. T needs no `==`: a change that shows nothing new costs a view, and no
 * operation on the renderer.
 *
 * Thread safety: none. Used from the app's executor, which runs one thing at a time.
 */
template <class T>
class state {
public:
  using value_type = T;

  state() = default;

  // Implicit, so that a component can give a default value: `state<int> count{1};`.
  state(T value) : value_(std::move(value)) {}

  // Links do not follow a copy: what it holds is the value of one component.
  state(state const &other) : value_(other.value_) {}

  state(state &&other) noexcept : value_(std::move(other.value_)) {}

  // Not from another state: it changes through set(), `=` from a value or update(), which tell of it.
  state &operator=(state const &) = delete;
  state &operator=(state &&) = delete;

  ~state() = default;

  state &operator=(T value) {
    set(std::move(value));
    return *this;
  }

  [[nodiscard]] T const &get() const noexcept QUOLL_LIFETIMEBOUND { return value_; }

  [[nodiscard]] T const &operator*() const noexcept QUOLL_LIFETIMEBOUND { return value_; }

  [[nodiscard]] T const *operator->() const noexcept QUOLL_LIFETIMEBOUND { return &value_; }

  void set(T value) {
    value_ = std::move(value);
    notify();
  }

  /** @brief Changes the value in place: `notes.update([](auto &all) { all.push_back(note); });`. */
  template <class F>
    requires std::invocable<F &, T &>
  void update(F change) {
    change(value_);
    notify();
  }

  /** @brief For the app of the component holding it. */
  void link(detail::state_link to) noexcept { link_ = to; }

private:
  void notify() { detail::tell(link_); }

  T value_{};
  detail::state_link link_{};
};

static_assert(detail::LinkedMember<state<int>>);

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_STATE_HPP
