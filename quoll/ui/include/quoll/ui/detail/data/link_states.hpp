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

#ifndef QUOLL_UI_DETAIL_DATA_LINK_STATES_HPP
#define QUOLL_UI_DETAIL_DATA_LINK_STATES_HPP

#include <quoll/ui/component/component_with_props.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/props_member.hpp>
#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/detail/for_each_member.hpp>

#include <type_traits>

namespace quoll::ui::detail {

template <class C>
void link_states(C &component, state_link link) {
  // Checked here, where every component, context and app passes: what reads props is in a component that has them.
  static_assert(
      ComponentWithProps<C> || !any_member<C, is_props_member>,
      "quoll::ui::form_state: in a component with props, one of whose fields it edits"
  );

  for_each_member(component, [&link](auto &member) {
    if constexpr (LinkedMember<std::remove_cvref_t<decltype(member)>>) {
      member.link(link);
    }
  });
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_LINK_STATES_HPP
