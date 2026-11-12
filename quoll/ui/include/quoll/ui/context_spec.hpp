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

#ifndef QUOLL_UI_CONTEXT_SPEC_HPP
#define QUOLL_UI_CONTEXT_SPEC_HPP

#include <quoll/ui/detail/held_component.hpp>

#include <functional>
#include <utility>

namespace quoll::ui {

namespace detail {

struct context_table;

struct context_spec_access;

} // namespace detail

class context_spec;

template <class T>
[[nodiscard]] constexpr context_spec provide() noexcept;

/**
 * @brief A context an app makes and holds, which only `provide<T>()`, in <quoll/ui/provide.hpp>, and Quoll's own, as
 * routing(), make: see shared.
 */
class context_spec {
public:
  [[nodiscard]] detail::context_table const &table() const noexcept { return *table_; }

  // What makes the context: its table's, unless the spec gave its own.
  [[nodiscard]] detail::held_component make() const;

private:
  template <class T>
  friend constexpr context_spec provide() noexcept;

  friend struct detail::context_spec_access;

  explicit constexpr context_spec(detail::context_table const &table) noexcept : table_(&table) {}

  context_spec(detail::context_table const &table, std::function<detail::held_component()> make) noexcept
      : table_(&table), make_(std::move(make)) {}

  detail::context_table const *table_;
  std::function<detail::held_component()> make_{};
};

namespace detail {

// For Quoll's own contexts, made otherwise than value-made.
struct context_spec_access {
  [[nodiscard]] static context_spec made_with(context_table const &table, std::function<held_component()> make) {
    return {table, std::move(make)};
  }
};

} // namespace detail

} // namespace quoll::ui

#endif // QUOLL_UI_CONTEXT_SPEC_HPP
