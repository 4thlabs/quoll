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

#include "sales_chart.hpp"

#include "product.hpp"

#include <quoll/ui.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>

namespace dashboard_web {

using namespace quoll::ui;

// What the chart emits as a point is clicked. Outside an unnamed namespace, where Glaze cannot reflect a type under
// Clang.
struct point_selected {
  int index = 0;
};

namespace {

// Its props are the sales themselves, as JSON: {"months": [...], "units": [...]}.
constexpr auto line_chart = widget<products::monthly_sales>("../widgets/line_chart.js", "canvas");

} // namespace

element sales_chart::view(props const &given) {
  // Sales come from the server, as months and units: only the months that have both are points of this chart.
  auto const points = std::min(given.sales.months.size(), given.sales.units.size());

  // The index comes from JavaScript: one out of those is no point.
  auto const select = [this, points](point_selected event) {
    if (event.index >= 0 && static_cast<std::size_t>(event.index) < points) {
      selected = std::optional<std::size_t>{static_cast<std::size_t>(event.index)};
    }
  };

  auto const figure = [this, &given] {
    auto const month = **selected;

    return p(std::format("{}: {} units", given.sales.months[month], given.sales.units[month]));
  };

  // A month chosen in sales that were longer shows nothing.
  auto const shows_figure = selected->has_value() && **selected < points;

  auto const chart = line_chart(given.sales) | on("point_selected", select);

  auto const shown = section({
      h2("Sales") | tw("text-lg font-semibold"),
      div({chart}) | tw("relative h-64"),
      when(shows_figure, figure),
  });

  return shown | tw("flex flex-col gap-2");
}

} // namespace dashboard_web
