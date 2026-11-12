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

// One product of the list: a link to its page.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_ROW_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_ROW_HPP

#include "product.hpp"
#include "product_page.hpp"

#include <quoll/ui.hpp>

#include <cstdint>
#include <string>

namespace dashboard_web {

using namespace quoll::ui;

struct product_row {
  struct props {
    products::product item;
    bool selected = false;

    // A row whose product and selection are unchanged is not viewed again.
    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) {
    auto const stock =
        given.item.stock == 0 ? std::string{"out of stock"} : std::to_string(given.item.stock) + " in stock";

    auto const name = span(given.item.name) | tw("flex-1 text-left font-medium");
    auto const category = span(given.item.category) | tw("badge badge-ghost");
    auto const in_stock = span(stock) | tw("whitespace-nowrap text-right text-sm opacity-70");

    // A link to its page: the address follows, and the back button comes back.
    auto const row = link_to<product_page>({.id = given.item.id}, {name, category, in_stock});

    auto const shown = row | tw("flex w-full items-center gap-3 rounded-box px-4 py-2 hover:bg-base-200") |
                       tw_if(given.selected, "bg-base-200");

    return li({shown});
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_ROW_HPP
