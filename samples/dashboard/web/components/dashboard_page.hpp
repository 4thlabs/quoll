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
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dashboard_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::effect;
using quoll::ui::element;
using quoll::ui::keyed_action;
using quoll::ui::run_status;
using quoll::ui::state;

struct dashboard_page {
  state<std::vector<products::product>> items;
  // Why the products could not be loaded, in the alert.
  state<std::string> problem;
  // The product chosen, and its sales, loaded again as it changes, the load before stopped, as React's useEffect.
  state<std::optional<std::int64_t>> selected;
  effect<products::monthly_sales> sales{selected, products_client::sales};
  // The saves of each product, once at a time, which its detail shows: under way, or why the last one failed.
  keyed_action<std::int64_t, products::product(products::product_update)> saves{products_client::update};

  task<> mounted() {
    try {
      items = co_await products_client::list();
    } catch (std::exception const &failure) {
      problem = told(failure, "the products did not load");
    } catch (...) {
      problem = std::string{"the products did not load"};
    }
  }

  // Its sales follow.
  void select(std::int64_t id) { selected = std::optional<std::int64_t>{id}; }

  // Whether it was saved: a save of that product under way refuses this one, and its failure stays with the saves.
  task<bool> save(std::int64_t id, products::product_update changes) {
    auto const saved = co_await saves.run(id, std::move(changes));

    if (!saved) {
      co_return false;
    }

    auto const replace = [given = *saved](std::vector<products::product> &all) {
      std::ranges::replace_if(all, [&given](auto const &item) { return item.id == given.id; }, given);
    };

    items.update(replace);
    co_return true;
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

    // Its sales load again on a click, as React runs an effect again: choosing the same product changes nothing.
    auto const retry = [this] { sales.reload(); };

    // Done, with its sales, or why they did not load and a way to try again. Keyed by the product, as the detail:
    // another product chosen is another chart.
    auto const sales_done = [this, &item_chosen, &retry] {
      auto const &result = sales.result();

      if (!result) {
        auto const failed = div({
            error_text(told(result.error(), "the sales did not load")),
            button("Try again") | tw("btn btn-sm self-start") | on_click(retry),
        });

        return failed | tw("flex flex-col gap-2");
      }

      return component<sales_chart>({.sales = *result}) | key(item_chosen->id);
    };

    auto const detail = [&] {
      auto const &item = *item_chosen;
      // A failed save stays with the product until its next.
      auto const save = saves.status(item.id);
      auto const saving = save == run_status::running;
      auto const save_failure = save == run_status::done && !saves.result(item.id)
                                    ? told(saves.result(item.id).error(), "the save failed")
                                    : std::string{};

      auto const shown = div({
          // Keyed by the product: another product chosen is another detail, with nothing typed.
          component<product_detail>({
              .item = item,
              .saving = saving,
              .save_failure = save_failure,
              .on_save = on_save,
          }) | key(item.id),
          when(sales.status() == run_status::running, loading),
          when(sales.status() == run_status::done, sales_done),
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
