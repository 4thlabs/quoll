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

// The page: the products and the one chosen, its detail and its sales. Its calls to the server are in
// products_client.cpp, so that a change here does not compile REST and JSON again.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP

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
#include <utility>
#include <vector>

namespace dashboard_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::element;
using quoll::ui::state;

struct dashboard_page {
  state<std::vector<products::product>> items;
  state<std::optional<std::int64_t>> selected;
  state<std::optional<products::monthly_sales>> sales;
  state<std::string> problem;

  task<> mounted() {
    try {
      items = co_await products_client::list();
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
    }
  }

  task<> select(std::int64_t id) {
    selected = std::optional<std::int64_t>{id};
    sales.set({});

    try {
      auto found = co_await products_client::sales(id);

      // Another product chosen meanwhile has its own sales on the way, and says how they went.
      if (*selected == id) {
        sales = std::optional<products::monthly_sales>{std::move(found)};
        problem.set({});
      }
    } catch (std::exception const &failure) {
      if (*selected == id) {
        problem = std::string{failure.what()};
      }
    }
  }

  task<bool> save(std::int64_t id, products::product_update changes) {
    try {
      auto const saved = co_await products_client::update(id, std::move(changes));

      auto const replace = [&saved](std::vector<products::product> &all) {
        std::ranges::replace_if(all, [&saved](auto const &item) { return item.id == saved.id; }, saved);
      };

      items.update(replace);
      problem.set({});
      co_return true;
    } catch (std::exception const &failure) {
      problem = std::string{failure.what()};
      co_return false;
    }
  }

  // The product chosen, while the list holds it.
  [[nodiscard]] products::product const *chosen() const {
    if (!selected->has_value()) {
      return nullptr;
    }

    auto const found = std::ranges::find(*items, **selected, &products::product::id);

    return found == items->end() ? nullptr : &*found;
  }

  element view() {
    using namespace quoll::ui;

    // One callback of each, which every row and detail shares.
    callback<void(std::int64_t)> const on_select = [this](std::int64_t id) { return select(id); };

    callback<bool(std::int64_t, products::product_update)> const on_save =
        [this](std::int64_t id, products::product_update changes) { return save(id, std::move(changes)); };

    auto const alert = [this] { return p(*problem) | tw("alert alert-error") | attribute("role", "alert"); };

    auto const none = [] { return p("Choose a product") | tw("opacity-70"); };

    auto const loading = [] { return p("Loading its sales") | tw("opacity-70"); };

    auto const *const item_chosen = chosen();

    auto const detail = [&] {
      auto const &item = *item_chosen;

      // Keyed by the product: another product chosen is another detail, with nothing typed, and another chart.
      auto const chart = [this, &item] { return component<sales_chart>({.sales = **sales}) | key(item.id); };

      auto const shown = div({
          component<product_detail>({.item = item, .on_save = on_save}) | key(item.id),
          // Until they come, or a failure the alert shows, which choosing the product again tries anew.
          when(!sales->has_value() && problem->empty(), loading),
          when(sales->has_value(), chart),
      });

      return shown | tw("flex flex-col gap-8");
    };

    auto const page = main_({
        h1("Products") | tw("text-4xl font-bold"),
        when(!problem->empty(), alert),
        div({
            component<product_list>({.items = *items, .selected = *selected, .on_select = on_select}),
            when(item_chosen == nullptr, none),
            when(item_chosen != nullptr, detail),
        }) | tw("grid gap-8 md:grid-cols-2"),
    });

    return page | tw("mx-auto flex max-w-5xl flex-col gap-6 p-8");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_DASHBOARD_PAGE_HPP
