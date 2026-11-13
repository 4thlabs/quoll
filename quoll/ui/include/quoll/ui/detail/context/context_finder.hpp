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

#ifndef QUOLL_UI_DETAIL_CONTEXT_CONTEXT_FINDER_HPP
#define QUOLL_UI_DETAIL_CONTEXT_CONTEXT_FINDER_HPP

#include <quoll/async/detail/type_id.hpp>

#include <memory>

namespace quoll::ui::detail {

struct app_link;
class context_entry;

// A context found, and what holds what provides it, a component above, for as long as the one using it lives: none for
// the app's, which outlives its components.
struct found_context {
  context_entry *entry = nullptr;
  std::shared_ptr<void const> keeper{};
};

// Where a component finds its contexts, by type: those the components above it provide, nearest first, then those the
// app's own component provides, then the app's; none for a view written once (to_html).
struct context_finder {
  std::shared_ptr<app_link> app{};
  void const *owner = nullptr;
  context_entry *(*find)(void const *owner, async::detail::type_id type) noexcept = nullptr;
  // The component placing the one it finds for, from which those above are walked: none for the app's own.
  std::shared_ptr<void const> from{};
  found_context (*find_above)(std::shared_ptr<void const> const &from, async::detail::type_id type) noexcept = nullptr;

  // The context of that type, or none.
  [[nodiscard]] found_context operator()(async::detail::type_id type) const noexcept {
    if (from && find_above != nullptr) {
      if (auto found = find_above(from, type); found.entry != nullptr) {
        return found;
      }
    }

    return {.entry = find == nullptr ? nullptr : find(owner, type), .keeper = nullptr};
  }
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CONTEXT_CONTEXT_FINDER_HPP
