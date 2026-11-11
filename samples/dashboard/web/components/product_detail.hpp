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

// The product chosen, to edit: what is typed stays here until it is saved, the product itself is the page's.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP

#include "field.hpp"
#include "product.hpp"

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace dashboard_web {

using quoll::async::task;
using quoll::ui::callback;
using quoll::ui::element;
using quoll::ui::state;

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
    // Whether it was saved: the page says why not.
    callback<bool(std::int64_t, products::product_update)> on_save;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  // What is typed in each field, none until one changes: the field shows the product's value meanwhile.
  state<std::optional<std::string>> name;
  state<std::optional<std::string>> price;
  state<std::optional<std::string>> stock;
  state<std::string> problem;

  task<> save(products::product item, callback<bool(std::int64_t, products::product_update)> on_save) {
    auto const new_price = number_in<double>(price->value_or(std::format("{}", item.price)));
    auto const new_stock = number_in<int>(stock->value_or(std::to_string(item.stock)));

    if (!new_price || !new_stock) {
      problem = std::string{"The price and the stock are numbers"};
      co_return;
    }

    problem.set({});

    products::product_update changes{.name = name->value_or(item.name), .price = *new_price, .stock = *new_stock};
    bool const saved = co_await on_save(item.id, std::move(changes));

    // Saved, the product given next holds what was typed.
    if (saved) {
      name.set({});
      price.set({});
      stock.set({});
    }
  }

  element view(props const &given) {
    using namespace quoll::ui;

    auto const edit = [](state<std::optional<std::string>> &typed) {
      return [&typed](std::string value) { typed = std::optional<std::string>{std::move(value)}; };
    };

    auto const send = [this, &given] { return save(given.item, given.on_save); };

    auto const fail = [this] { return p(*problem) | tw("text-sm text-error"); };

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
        when(!problem->empty(), fail),
        button("Save") | tw("btn btn-primary self-start") | on_click(send),
    });

    return form | tw("flex flex-col gap-3");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
