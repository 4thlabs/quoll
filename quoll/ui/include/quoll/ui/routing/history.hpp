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

#ifndef QUOLL_UI_ROUTING_HISTORY_HPP
#define QUOLL_UI_ROUTING_HISTORY_HPP

#include <quoll/async/detail/type_id.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::ui {

/**
 * @brief The history an app's routing goes through: its address now, and what pushes, replaces and goes back, which
 * the browser's is (quoll::emscripten::browser_history()), or a memory_history.
 *
 * A non-owning wrapper: what it refers to outlives the app. `listen` is told the address each time it changes without
 * the app asking, as the browser's back button does. Thread safety: none, as the app using it, on its executor.
 */
class history_ref {
public:
  struct table {
    // What it refers to, for target().
    async::detail::type_id type;
    std::string (*current)(void *self);
    void (*push)(void *self, std::string_view href);
    void (*replace)(void *self, std::string_view href);
    void (*back)(void *self);
    void (*listen)(void *self, void *listener, void (*changed)(void *listener, std::string_view href));
    void (*unlisten)(void *self, void *listener);
  };

  history_ref(void *self, table const &functions) noexcept : self_(self), table_(&functions) {}

  [[nodiscard]] std::string current() const { return table_->current(self_); }

  void push(std::string_view href) const { table_->push(self_, href); }

  void replace(std::string_view href) const { table_->replace(self_, href); }

  void back() const { table_->back(self_); }

  void listen(void *listener, void (*changed)(void *listener, std::string_view href)) const {
    table_->listen(self_, listener, changed);
  }

  void unlisten(void *listener) const noexcept { table_->unlisten(self_, listener); }

  /** @brief What it refers to, as a T, or none when it refers to another type. */
  template <class T>
  [[nodiscard]] T *target() const noexcept {
    return table_->type == async::detail::type_id_of<T>() ? static_cast<T *>(self_) : nullptr;
  }

private:
  void *self_;
  table const *table_;
};

/**
 * @brief A history in memory, for a program with no browser, and for tests: its addresses, the one now, and its
 * listener, which back() tells, as the browser's back button does.
 *
 * Thread safety: none, as the app using it, on its executor.
 */
class memory_history {
public:
  explicit memory_history(std::string start = "/");

  memory_history(memory_history const &) = delete;
  memory_history(memory_history &&) = delete;
  memory_history &operator=(memory_history const &) = delete;
  memory_history &operator=(memory_history &&) = delete;

  ~memory_history() = default;

  [[nodiscard]] history_ref ref() noexcept;

  [[nodiscard]] std::string const &current() const noexcept { return entries_[at_]; }

  void push(std::string_view href);
  void replace(std::string_view href);
  // The address before, told to its listener: none before the first.
  void back();

  /** @brief How many addresses it holds, the ones after the current included. */
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  std::vector<std::string> entries_;
  std::size_t at_ = 0;
  void *listener_ = nullptr;
  void (*changed_)(void *listener, std::string_view href) = nullptr;
};

} // namespace quoll::ui

#endif // QUOLL_UI_ROUTING_HISTORY_HPP
