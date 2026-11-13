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

#ifndef QUOLL_UI_DETAIL_DATA_RUN_EFFECTS_HPP
#define QUOLL_UI_DETAIL_DATA_RUN_EFFECTS_HPP

#include <quoll/ui/detail/data/effect_member.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/for_each_member.hpp>

#include <exception>
#include <type_traits>

namespace quoll::ui::detail {

// After a view of `component`: each of its effects whose dependency changed starts, through `start`. One that cannot
// start leaves the others to start all the same, and what it threw is reported, each failure, or, with no reporter,
// the first thrown once they have.
template <class C>
void run_effects(C &component, effect_starter const &start) {
  std::exception_ptr first;
  for_each_member(component, [&](auto &member) {
    if constexpr (EffectMember<std::remove_cvref_t<decltype(member)>>) {
      try {
        member.after_view(start);
      } catch (...) {
        if (start.report != nullptr) {
          start.report(start.owner, std::current_exception());
        } else if (!first) {
          first = std::current_exception();
        }
      }
    }
  });

  if (first) {
    std::rethrow_exception(first);
  }
}

// Before the first view of a component, or a context, just made: the targets of its effects whose first run will start
// say they are loading. Nothing starts, and nothing throws.
inline void first_effects(void (*effects)(void *, effect_starter const &), void *component) noexcept {
  if (effects != nullptr) {
    effects(component, effect_starter{.first = true});
  }
}

// What a table holds to start the effects of `C`: none for a component that has none.
template <class C>
[[nodiscard]] constexpr auto effects_of() noexcept -> void (*)(void *, effect_starter const &) {
  if constexpr (any_member<C, is_effect_member>) {
    return [](void *component, effect_starter const &start) { run_effects(*static_cast<C *>(component), start); };
  } else {
    return nullptr;
  }
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_RUN_EFFECTS_HPP
