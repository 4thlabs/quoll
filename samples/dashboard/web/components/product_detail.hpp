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

// The product shown, to edit: what is typed stays here until it is saved, the product itself is the catalog's.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP

#include "catalog.hpp"
#include "error_text.hpp"
#include "labelled.hpp"
#include "product.hpp"

#include <quoll/ui.hpp>

#include <string>

namespace dashboard_web {

using namespace quoll::ui;

struct product_detail {
  struct props {
    products::product item;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  shared<catalog> products;

  // The product edited: the one given, with what is typed.
  form_state<&props::item> form;

  // Saves the product edited, once at a time; the save is the catalog's, which goes on as the detail is left.
  action<&catalog::save> save{products};

  element view(props const &given) {
    auto const not_numbers = [] { return error_text("The price and the stock are numbers"); };
    auto const save_failed = [this] { return error_text("Not saved: " + told(save->error, "the save failed")); };

    // The fields stay open while a save is on its way: what is typed meanwhile is kept for the next.
    auto const shown = section({
        h2(given.item.name) | tw("text-2xl font-semibold"),
        labelled("Name", input() | bind(form, &products::product::name)),
        labelled("Price", input() | bind(form, &products::product::price)),
        labelled("Stock", input() | bind(form, &products::product::stock)),
        // What is typed wrong first: the last save's failure is of what was sent then.
        when(!form.valid(), not_numbers),
        when(form.valid() && save->error != nullptr, save_failed),
        button(save->loading ? "Saving…" : "Save") | disabled(save->loading || !form.valid()) |
            on_click(save.with(form)) | tw("btn btn-primary self-start"),
    });

    return shown | tw("flex flex-col gap-3");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_PRODUCT_DETAIL_HPP
