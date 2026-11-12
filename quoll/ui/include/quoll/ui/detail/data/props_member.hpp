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

#ifndef QUOLL_UI_DETAIL_DATA_PROPS_MEMBER_HPP
#define QUOLL_UI_DETAIL_DATA_PROPS_MEMBER_HPP

#include <quoll/ui/detail/for_each_member.hpp>

#include <concepts>
#include <type_traits>

namespace quoll::ui::detail {

// What its component gives its props before each of its views, as a form_state takes the value it edits from them:
// `props_type` the props it reads, its component's.
template <class T>
concept PropsMember = requires { typename T::props_type; } &&
                      requires(T &member, typename T::props_type const &given) { member.before_view(given); };

// PropsMember, as a trait for any_member().
template <class T>
struct is_props_member : std::bool_constant<PropsMember<T>> {};

// Gives `given`, the props of the view of `component` about to run, to those of its members that read them.
template <class C, class P>
void give_props(C &component, P const &given) {
  for_each_member(component, [&given](auto &member) {
    using held = std::remove_cvref_t<decltype(member)>;

    if constexpr (PropsMember<held>) {
      static_assert(
          std::same_as<typename held::props_type, P>,
          "quoll::ui::form_state: of a field of the props of its own component"
      );
      member.before_view(given);
    }
  });
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_PROPS_MEMBER_HPP
