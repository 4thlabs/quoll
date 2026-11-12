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

// The page: the products, from the catalog, and the one chosen, its detail and its sales. Its calls to the server are
// in products_client.cpp, so that a change here does not compile REST and JSON again.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP

#include "catalog.hpp"
#include "error_text.hpp"
#include "product.hpp"
#include "product_detail.hpp"
#include "product_list.hpp"
#include "products_client.hpp"
#include "sales_chart.hpp"

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dashboard_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::effect;
using quoll::ui::element;
using quoll::ui::shared;
using quoll::ui::state;

// The sales of a product, with the product they are of: those of the product before stay as the next load, as a state
// does, and the view shows a chart for the product chosen only.
[[nodiscard]] inline task<std::tuple<std::int64_t, products::monthly_sales>> sales_of(std::int64_t id) {
  auto sales = co_await products_client::sales(id);

  co_return {id, std::move(sales)};
}

struct dashboard_page {
  // The products, which its list and its detail read too.
  shared<catalog> products;

  // The product chosen, and its sales, whether they are loading and why they did not load: loaded again as it changes,
  // the load before stopped, as React's useEffect.
  state<std::optional<std::int64_t>> selected;
  state<std::tuple<std::int64_t, products::monthly_sales, bool, std::exception_ptr>> sales;
  effect load_sales{selected, sales_of, sales};

  // Its sales follow.
  void select(std::int64_t id) { selected = std::optional<std::int64_t>{id}; }

  // The product chosen, while the list holds it.
  [[nodiscard]] products::product const *chosen() const {
    if (!selected->has_value()) {
      return nullptr;
    }

    auto const &all = std::get<0>(*products->items);
    auto const found = std::ranges::find(all, **selected, &products::product::id);

    return found == all.end() ? nullptr : &*found;
  }

  element view() {
    using namespace quoll::ui;

    auto const &problem = std::get<2>(*products->items);

    // One callback, which every row shares.
    callback<void(std::int64_t)> const on_select = [this](std::int64_t id) { return select(id); };

    auto const alert = [&problem] {
      return p(told(problem, "the products did not load")) | tw("alert alert-error") | attribute("role", "alert");
    };

    auto const none = [] { return p("Choose a product") | tw("opacity-70"); };

    auto const *const item_chosen = chosen();

    // Its sales load again on a click, as React runs an effect again: choosing the same product changes nothing.
    auto const retry = [this] { load_sales.reload(); };

    // Loading, or why they did not load and a way to try again, or the chart of the product chosen. Keyed by the
    // product, as the detail: another product chosen is another chart.
    auto const sales_shown = [this, &item_chosen, &retry] {
      auto const &[sales_of_id, monthly, loading, failure] = *sales;

      if (loading) {
        return p("Loading its sales") | tw("opacity-70");
      }

      if (failure) {
        auto const failed = div({
            error_text(told(failure, "the sales did not load")),
            button("Try again") | tw("btn btn-sm self-start") | on_click(retry),
        });

        return failed | tw("flex flex-col gap-2");
      }

      if (sales_of_id != item_chosen->id) {
        return div({});
      }

      return component<sales_chart>({.sales = monthly}) | key(item_chosen->id);
    };

    auto const detail = [&] {
      auto const &item = *item_chosen;

      auto const shown = div({
          // Keyed by the product: another product chosen is another detail, with nothing typed; a save on its way goes
          // on in the catalog.
          component<product_detail>({.item = item}) | key(item.id),
          sales_shown(),
      });

      return shown | tw("flex flex-col gap-8");
    };

    auto const page = main_({
        h1("Products") | tw("text-4xl font-bold"),
        when(problem != nullptr, alert),
        div({
            component<product_list>({.selected = *selected, .on_select = on_select}),
            when(item_chosen == nullptr, none),
            when(item_chosen != nullptr, detail),
        }) | tw("grid gap-8 md:grid-cols-2"),
    });

    return page | tw("mx-auto flex max-w-5xl flex-col gap-6 p-8");
  }
};

// What the page's app holds, which its main and its tests give alike: the catalog its components share.
[[nodiscard]] inline quoll::ui::app_options page_options() { return {.contexts = {quoll::ui::provide<catalog>()}}; }

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP
