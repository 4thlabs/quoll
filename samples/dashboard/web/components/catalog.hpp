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

// The products, which the page, its list and its detail share: loaded once, as the page first shows, and saved here,
// so that a save goes on as the detail asking for it goes, and every component showing the product shows it.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_CATALOG_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_CATALOG_HPP

#include "product.hpp"
#include "products_client.hpp"

#include <quoll/async/task.hpp>
#include <quoll/ui.hpp>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace dashboard_web {

using namespace quoll::ui;

struct catalog {
  // The products, whether they are loading, and why they did not load: loading from the start, their load due.
  async_state<std::vector<products::product>> items;
  effect load{std::tie(), products_client::list, items};

  // Saves a product, which the list then shows. Two details of one product may still send their saves together, the
  // last answer kept; what a context keeps per item, as a save of a product once at a time, is to settle
  // (docs/PLAN.md, step 6, Contexts).
  quoll::async::task<products::product> save(std::int64_t id, products::product_update changes) {
    auto saved = co_await products_client::update(id, std::move(changes));

    auto const replace = [&saved](auto &held) {
      std::ranges::replace_if(held.value, [&saved](auto const &item) { return item.id == saved.id; }, saved);
    };

    items.update(replace);
    co_return saved;
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_CATALOG_HPP
