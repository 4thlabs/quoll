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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dashboard_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::element;
using quoll::ui::state;

// How the saves of a product go: none is shown when it is not under way and the last one did not fail.
struct save_status {
  bool under_way = false;
  std::string failure;
};

struct dashboard_page {
  state<std::vector<products::product>> items;
  state<std::optional<std::int64_t>> selected;
  state<std::optional<products::monthly_sales>> sales;
  // Why the products could not be loaded, in the alert.
  state<std::string> problem;
  // Why the sales of the product chosen could not be loaded, in its detail.
  state<std::string> sales_problem;
  // The saves of each product, which its detail shows: under way, or why the last one failed.
  state<std::map<std::int64_t, save_status>> saves;
  // The sales asked for: a plain member, as no view shows it.
  std::uint64_t sales_asked = 0;

  task<> mounted() {
    try {
      items = co_await products_client::list();
    } catch (std::exception const &failure) {
      problem = told(failure, "the products did not load");
    } catch (...) {
      problem = std::string{"the products did not load"};
    }
  }

  task<> select(std::int64_t id) {
    auto const asked = ++sales_asked;

    selected = std::optional<std::int64_t>{id};
    sales.set({});
    sales_problem.set({});

    std::optional<products::monthly_sales> found;
    std::string failure;

    try {
      found = co_await products_client::sales(id);
    } catch (std::exception const &error) {
      failure = told(error, "the sales did not load");
    } catch (...) {
      failure = "the sales did not load";
    }

    // A product chosen meanwhile, or this one chosen again, has its own sales on the way, and says how they went.
    if (asked != sales_asked) {
      co_return;
    }

    if (found) {
      sales = std::move(found);
    } else {
      sales_problem = failure;
    }
  }

  // Whether it was saved. One save of a product at a time, so that two are never answered out of order: its detail
  // shows it is on its way, and one clicked twice in a turn sends once.
  task<bool> save(std::int64_t id, products::product_update changes) {
    if (status_of(id).under_way) {
      co_return false;
    }

    // Under way, the failure of the last one gone.
    saves.update([id](auto &all) { all[id] = {.under_way = true, .failure = {}}; });

    constexpr std::string_view not_saved = "the save failed";

    // However it ends but for a stop, which takes the page with it, the save is over for its detail, whichever product
    // shows by then.
    std::optional<products::product> saved;
    std::string failure;

    try {
      saved = co_await products_client::update(id, std::move(changes));
    } catch (std::exception const &error) {
      failure = told(error, not_saved);
    } catch (...) {
      failure = not_saved;
    }

    saves.update([id, &saved, &failure](auto &all) {
      if (saved) {
        all.erase(id);
      } else {
        all[id] = {.under_way = false, .failure = failure};
      }
    });

    if (!saved) {
      co_return false;
    }

    auto const replace = [&saved](std::vector<products::product> &all) {
      std::ranges::replace_if(all, [&saved](auto const &item) { return item.id == saved->id; }, *saved);
    };

    items.update(replace);
    co_return true;
  }

  // How the saves of a product go: none under way and no failure when it has none.
  [[nodiscard]] save_status status_of(std::int64_t id) const {
    auto const found = saves->find(id);

    return found == saves->end() ? save_status{} : found->second;
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

    // Choosing the product again tries anew.
    auto const sales_failed = [this] { return error_text(*sales_problem); };

    auto const *const item_chosen = chosen();

    auto const detail = [&] {
      auto const &item = *item_chosen;
      auto const status = status_of(item.id);

      // Keyed by the product: another product chosen is another detail, with nothing typed, and another chart.
      auto const chart = [this, &item] { return component<sales_chart>({.sales = **sales}) | key(item.id); };

      auto const shown = div({
          component<product_detail>({
              .item = item,
              .saving = status.under_way,
              .save_failure = status.failure,
              .on_save = on_save,
          }) | key(item.id),
          when(!sales->has_value() && sales_problem->empty(), loading),
          when(!sales_problem->empty(), sales_failed),
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
