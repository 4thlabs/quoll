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

// What the page of a product shows of it: its detail, to edit, and its sales, in a chart. Keyed by the product, so
// that another product is another view, its states, what is typed and its sales, its own from the start.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_VIEW_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_VIEW_HPP

#include "error_text.hpp"
#include "product.hpp"
#include "product_detail.hpp"
#include "products_client.hpp"
#include "sales_chart.hpp"

#include <quoll/ui.hpp>

#include <cstdint>

namespace dashboard_web {

using namespace quoll::ui;

struct product_view_props {
  // The product, and its id, which its sales load for: a field of its own, as an effect compares it, and the product
  // changes as it is saved, which would load them again.
  std::int64_t id = 0;
  products::product item;

  [[nodiscard]] bool operator==(product_view_props const &) const = default;
};

// A function component: its sales, whether they are loading, and why they did not load, are its hooks.
inline element product_view(product_view_props const &given) {
  auto &sales = use_async_state<products::monthly_sales>();
  auto &load_sales = use_effect(given.id, products_client::sales, sales);

  // Loading, or why they did not load and a way to try again, or their chart.
  auto const sales_shown = [&]() -> element {
    if (sales->loading) {
      return p("Loading its sales") | tw("opacity-70");
    }

    if (sales->error) {
      // As React runs an effect again: the product does not change.
      auto const retry = [&load_sales] { load_sales.reload(); };

      auto const failed = div({
          error_text(told(sales->error, "the sales did not load")),
          button("Try again") | tw("btn btn-sm self-start") | on_click(retry),
      });

      return failed | tw("flex flex-col gap-2");
    }

    return component<sales_chart>({.sales = sales->value});
  };

  auto const shown = div({
      component<product_detail>({.item = given.item}),
      sales_shown(),
  });

  return shown | tw("flex flex-col gap-8");
}

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_VIEW_HPP
