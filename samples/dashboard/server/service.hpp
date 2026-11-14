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

// What the server does with each endpoint of the contract, over products in memory.

#ifndef QUOLL_SAMPLES_DASHBOARD_SERVER_SERVICE_HPP
#define QUOLL_SAMPLES_DASHBOARD_SERVER_SERVICE_HPP

#include "products_api.hpp"

#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace products {

// The runtime runs the requests on one thread: no lock.
class service {
public:
  service() {
    std::int64_t id = 1;

    for (auto const &[name, category, price, stock] : catalog) {
      products_.emplace(id, product{.id = id, .name = name, .category = category, .price = price, .stock = stock});
      ++id;
    }
  }

  std::vector<product> operator()(list) const {
    std::vector<product> all;

    for (auto const &[id, item] : products_) {
      all.push_back(item);
    }

    return all;
  }

  product operator()(update, std::int64_t id, product_update changes) {
    auto &item = find(*this, id);

    if (changes.name.empty() || changes.price < 0 || changes.stock < 0) {
      throw quoll::http::http_error{
          quoll::http::status::unprocessable_content, "a product needs a name, and a price and a stock of zero at least"
      };
    }

    item.name = std::move(changes.name);
    item.price = changes.price;
    item.stock = changes.stock;
    return item;
  }

  // Twelve months of made-up sales, the same for a product each time.
  monthly_sales operator()(sales, std::int64_t id) const {
    static constexpr std::array<double, 12> season{3, 4, 6, 7, 9, 12, 14, 13, 10, 8, 6, 11};
    static constexpr std::array<char const *, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    auto const &item = find(*this, id);
    monthly_sales found;

    for (std::size_t month = 0; month < months.size(); ++month) {
      found.months.emplace_back(months[month]);
      found.units.push_back(season[(month + static_cast<std::size_t>(item.id)) % season.size()] * 10);
    }

    return found;
  }

private:
  struct entry {
    char const *name;
    char const *category;
    double price;
    int stock;
  };

  static constexpr std::array<entry, 6> catalog{{
      {"Espresso beans", "Coffee", 12.5, 40},
      {"Filter beans", "Coffee", 10.0, 25},
      {"Green tea", "Tea", 8.0, 60},
      {"Black tea", "Tea", 7.5, 0},
      {"Grinder", "Gear", 49.0, 8},
      {"Kettle", "Gear", 35.0, 12},
  }};

  // The product of that id, const or not as the service is: a 404 problem when there is none.
  template <class Self>
  [[nodiscard]] static auto
  find(Self &self, std::int64_t id) -> std::conditional_t<std::is_const_v<Self>, product const &, product &> {
    auto const found = self.products_.find(id);

    if (found == self.products_.end()) {
      throw quoll::http::http_error{quoll::http::status::not_found, "no product " + std::to_string(id)};
    }

    return found->second;
  }

  std::map<std::int64_t, product> products_;
};

} // namespace products

#endif // QUOLL_SAMPLES_DASHBOARD_SERVER_SERVICE_HPP
