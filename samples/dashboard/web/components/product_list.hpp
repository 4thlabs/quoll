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

// The products, filtered by what is typed above them: the filter is the list's own state, the products the catalog's.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_LIST_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_LIST_HPP

#include "catalog.hpp"
#include "product.hpp"
#include "product_row.hpp"

#include <quoll/ui.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dashboard_web {

using namespace quoll::ui;

// Whether `text` holds `part`, whatever the case of their ASCII letters.
[[nodiscard]] inline bool holds(std::string_view text, std::string_view part) {
  auto const same = [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
  };

  return part.empty() || !std::ranges::search(text, part, same).empty();
}

struct product_list {
  struct props {
    // The product of the address, shown chosen.
    std::optional<std::int64_t> selected;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  shared<catalog> products;
  state<std::string> query;

  element view(props const &given) {
    auto const matches = [this](products::product const &item) {
      return holds(item.name, *query) || holds(item.category, *query);
    };

    std::vector<products::product> shown;

    std::ranges::copy_if(products->items->value, std::back_inserter(shown), matches);

    auto const row = [&given](products::product const &item) {
      auto const chosen = component<product_row>({
          .item = item,
          .selected = given.selected == item.id,
      });

      return chosen | key(item.id);
    };

    auto const nothing = [] { return p("No product matches") | tw("px-4 py-2 opacity-70"); };

    auto const panel = div({
        input() | bind(query) | tw("input input-bordered w-full") |
            attribute("placeholder", "Filter by name or category"),
        when(shown.empty(), nothing),
        ul({list(shown, row)}) | tw("flex flex-col gap-1"),
    });

    return panel | tw("flex flex-col gap-3");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_LIST_HPP
