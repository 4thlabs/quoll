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

// The page of a product, at /products/{id}: the product the catalog holds, or why it is not shown.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_PAGE_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_PAGE_HPP

#include "catalog.hpp"
#include "product.hpp"
#include "product_view.hpp"

#include <quoll/ui.hpp>

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace dashboard_web {

using namespace quoll::ui;

struct product_page {
  struct props {
    // The product of the address.
    std::int64_t id = 0;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  shared<catalog> products;

  element view(props const &given) {
    auto const &[all, loading, failure] = *products->items;
    auto const found = std::ranges::find(all, given.id, &products::product::id);

    // The layout tells why the products did not load.
    if (failure) {
      return div({});
    }

    if (found == all.end()) {
      return p(loading ? "Loading the product" : "No such product") | tw("opacity-70");
    }

    // Keyed by the product: another one is another view, with nothing typed and its own sales; a save on its way goes
    // on in the catalog.
    return component<product_view>({.id = found->id, .item = *found}) | key(found->id);
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_PAGE_HPP
