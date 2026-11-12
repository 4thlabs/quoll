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

// A labelled input showing a value it is given, and telling what is typed: the parent keeps the value.

#ifndef QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_FIELD_HPP
#define QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_FIELD_HPP

#include <quoll/ui.hpp>

#include <string>
#include <utility>

namespace dashboard_web {

using namespace quoll::ui;

struct field {
  struct props {
    std::string label;
    std::string value;
    callback<void(std::string)> on_change;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  element view(props const &given) {
    auto const change = [&given](std::string typed) { return given.on_change(std::move(typed)); };

    auto const shown = label({
        span(given.label) | tw("text-sm opacity-70"),
        input() | value(given.value) | tw("input input-bordered w-full") | on_input(change),
    });

    return shown | tw("flex flex-col gap-1");
  }
};

} // namespace dashboard_web

#endif // QUOLL_SAMPLES_DASHBOARD_WEB_COMPONENTS_FIELD_HPP
