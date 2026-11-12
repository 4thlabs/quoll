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

// The pages of the app and their addresses: the products, with a product's page under them.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PAGES_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PAGES_HPP

#include "catalog.hpp"
#include "product_page.hpp"
#include "products_layout.hpp"

#include <quoll/ui.hpp>

#include <string>
#include <utility>

namespace dashboard_web {

using namespace quoll::ui;

// At /products, beside the list: none chosen yet.
struct choose_product {
  element view() { return p("Choose a product") | tw("opacity-70"); }
};

// At an address no route matches.
struct missing_page {
  element view() {
    return div({
        p("No such page"),
        link_to<choose_product>("See the products") | tw("link"),
    });
  }
};

// The addresses of the app:
//
// /products        the list, with beside it:
// /products        - none chosen yet
// /products/{id}   - a product's page
// /                goes to /products
// anything else    missing_page
//
// One route a line, those under it indented, which clang-format would not keep.
// clang-format off
inline constexpr auto app_routes = routes(
    route<"/products", products_layout>(
        route<"", choose_product>(),
        route<"{id}", product_page>()),
    redirect<"/", "/products">(),
    not_found<missing_page>());
// clang-format on

// The app's own component: the page of the address.
struct app_shell {
  element view() { return main_({outlet()}) | tw("mx-auto flex max-w-5xl flex-col gap-6 p-8"); }
};

// What the app holds, through the browser's history in main.
[[nodiscard]] inline app_options page_options(history_ref history) {
  return {.contexts = {routing<app_routes>(history), provide<catalog>()}};
}

// The same, through a history in memory starting at `start`, as the tests have.
[[nodiscard]] inline app_options page_options(std::string start = "/products") {
  return {.contexts = {routing<app_routes>(std::move(start)), provide<catalog>()}};
}

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PAGES_HPP
