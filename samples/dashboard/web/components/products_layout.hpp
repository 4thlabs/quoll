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

// The products, at /products: their list, and beside it, in its outlet, the page of the address, a product's or none.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCTS_LAYOUT_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCTS_LAYOUT_HPP

#include "catalog.hpp"
#include "error_text.hpp"
#include "product_list.hpp"

#include <quoll/ui.hpp>

#include <cstdint>
#include <optional>
#include <tuple>

namespace dashboard_web {

using namespace quoll::ui;

struct products_layout {
  struct props {
    // The product of the address, /products/{id}, which the list shows chosen: none at /products.
    std::optional<std::int64_t> id;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  shared<catalog> products;

  element view(props const &given) {
    auto const &problem = std::get<2>(*products->items);

    auto const alert = [&problem] {
      return p(told(problem, "the products did not load")) | tw("alert alert-error") | attribute("role", "alert");
    };

    auto const columns = div({
        component<product_list>({.selected = given.id}),
        outlet(),
    });

    return div({
        h1("Products") | tw("text-4xl font-bold"),
        when(problem != nullptr, alert),
        columns | tw("grid gap-8 md:grid-cols-2"),
    });
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCTS_LAYOUT_HPP
