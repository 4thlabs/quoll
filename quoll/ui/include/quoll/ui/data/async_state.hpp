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

#ifndef QUOLL_UI_DATA_ASYNC_STATE_HPP
#define QUOLL_UI_DATA_ASYNC_STATE_HPP

#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>

#include <exception>

namespace quoll::ui {

/** @brief What an async_state holds: what a run gave, whether one is under way, and why the last failed. */
template <class T = void>
struct async_value {
  using value_type = T;

  T value{};
  bool loading = false;
  std::exception_ptr error{};

  [[nodiscard]] bool operator==(async_value const &) const = default;
};

/** @brief See async_value: for a run that gives nothing. */
template <>
struct async_value<void> {
  using value_type = void;

  bool loading = false;
  std::exception_ptr error{};

  [[nodiscard]] bool operator==(async_value const &) const = default;
};

/**
 * @brief A state what an effect or an action writes into: what its run gave, whether one is under way, and why the
 * last failed, by name.
 *
 * @code
 * struct product_view {
 *   async_state<products::monthly_sales> sales;
 *   effect load{&props::id, products_client::sales, sales};
 *
 *   element view(props const &) {
 *     if (sales->loading) {
 *       return p("Loading its sales");
 *     }
 *
 *     return sales->error ? p("No sales") : component<sales_chart>({.sales = sales->value});
 *   }
 * };
 * @endcode
 *
 * A state, as any: `*sales`, `sales->value`, `sales->loading`, `sales->error`. `async_state<>` holds no value, for an
 * action whose result goes to its caller alone. An effect's target is loading from the start when its first run will
 * start, as its component's first view comes before it; an action's, once a run starts.
 */
template <class T = void>
class async_state : public state<async_value<T>> {
public:
  using state<async_value<T>>::state;
  using state<async_value<T>>::operator=;
};

static_assert(detail::LinkedMember<async_state<int>>);

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_ASYNC_STATE_HPP
