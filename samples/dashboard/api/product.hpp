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

// The types of the products API, without the API: what the page's components use without compiling REST and JSON.

#ifndef QUOLL_SAMPLES_DASHBOARD_API_PRODUCT_HPP
#define QUOLL_SAMPLES_DASHBOARD_API_PRODUCT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace products {

struct product {
  std::int64_t id = 0;
  std::string name;
  std::string category;
  double price = 0;
  int stock = 0;

  [[nodiscard]] bool operator==(product const &) const = default;
};

/** @brief What the detail edits: a product but for its id and its category. */
struct product_update {
  std::string name;
  double price = 0;
  int stock = 0;

  [[nodiscard]] bool operator==(product_update const &) const = default;
};

/** @brief The units a product sold each month, the oldest first. */
struct monthly_sales {
  std::vector<std::string> months;
  std::vector<double> units;

  [[nodiscard]] bool operator==(monthly_sales const &) const = default;
};

} // namespace products

#endif // QUOLL_SAMPLES_DASHBOARD_API_PRODUCT_HPP
