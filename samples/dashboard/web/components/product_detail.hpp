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

// The product shown, to edit: what is typed stays here until it is saved, the product itself is the catalog's.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP

#include "catalog.hpp"
#include "error_text.hpp"
#include "field.hpp"
#include "product.hpp"

#include <quoll/ui.hpp>

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace dashboard_web {

using namespace quoll::ui;

// The number `text` holds whole, or nothing: from_chars reads "inf" and "nan" too, which JSON cannot hold.
template <class T>
[[nodiscard]] std::optional<T> number_in(std::string const &text) {
  T value{};
  auto const end = text.data() + text.size();
  auto const [stop, error] = std::from_chars(text.data(), end, value);

  if (error != std::errc{} || stop != end) {
    return std::nullopt;
  }

  if constexpr (std::floating_point<T>) {
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
  }

  return value;
}

struct product_detail {
  struct props {
    products::product item;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  // What is typed in each field, none until one changes: the field shows the product's value meanwhile.
  state<std::optional<std::string>> name;
  state<std::optional<std::string>> price;
  state<std::optional<std::string>> stock;
  shared<catalog> products;

  // The changes typed, or none while the price or the stock is not a number.
  [[nodiscard]] std::optional<products::product_update> changes(products::product const &item) const {
    auto const new_price = number_in<double>(price->value_or(std::format("{}", item.price)));
    auto const new_stock = number_in<int>(stock->value_or(std::to_string(item.stock)));

    if (!new_price || !new_stock) {
      return std::nullopt;
    }

    return products::product_update{.name = name->value_or(item.name), .price = *new_price, .stock = *new_stock};
  }

  // Saved, the product given next holds what was sent: the fields that hold it show it again, and those changed while
  // the save was on its way keep what they hold. Compared by hand, until quoll::ui has a form draft from the props
  // that tells what changed (docs/PLAN.md, step 6, and issue #163).
  void forget_saved(products::product const &saved) {
    if (*name == saved.name) {
      name = std::nullopt;
    }

    if (price->has_value() && number_in<double>(**price) == saved.price) {
      price = std::nullopt;
    }

    if (stock->has_value() && number_in<int>(**stock) == saved.stock) {
      stock = std::nullopt;
    }
  }

  // Once at a time: whether a save is on its way, and why the last one failed; the save is the catalog's, which goes on
  // as the detail is left.
  action<&catalog::save> save{products, [this](products::product const &saved) { forget_saved(saved); }};

  element view(props const &given) {
    auto const edit = [](state<std::optional<std::string>> &typed) {
      return [&typed](std::string value) { typed = std::optional<std::string>{std::move(value)}; };
    };

    auto const update = changes(given.item);

    auto const not_numbers = [] { return error_text("The price and the stock are numbers"); };
    auto const save_failed = [this] { return error_text("Not saved: " + told(save->error, "the save failed")); };

    // The fields stay open while a save is on its way: what is typed meanwhile is kept for the next. A click sends the
    // update, none while the fields are wrong; one while a save is on its way the action refuses.
    auto save_button = button(save->loading ? "Saving…" : "Save") | disabled(save->loading || !update);

    if (update) {
      save_button = std::move(save_button) | on_click(save.with(given.item.id, *update));
    }

    auto const form = section({
        h2(given.item.name) | tw("text-2xl font-semibold"),
        component<field>({.label = "Name", .value = name->value_or(given.item.name), .on_change = edit(name)}),
        component<field>({
            .label = "Price",
            .value = price->value_or(std::format("{}", given.item.price)),
            .on_change = edit(price),
        }),
        component<field>({
            .label = "Stock",
            .value = stock->value_or(std::to_string(given.item.stock)),
            .on_change = edit(stock),
        }),
        // What is typed wrong first: the last save's failure is of what was sent then.
        when(!update, not_numbers),
        when(update && save->error != nullptr, save_failed),
        save_button | tw("btn btn-primary self-start"),
    });

    return form | tw("flex flex-col gap-3");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
