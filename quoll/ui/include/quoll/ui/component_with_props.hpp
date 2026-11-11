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

#ifndef QUOLL_UI_COMPONENT_WITH_PROPS_HPP
#define QUOLL_UI_COMPONENT_WITH_PROPS_HPP

#include <quoll/ui/element.hpp>

#include <concepts>

namespace quoll::ui {

/**
 * @brief A component of a parent's view, which the parent gives its props on each of its views: an aggregate with
 * `struct props` and `element view(props const &)`, and its own state<T> members, kept from one view of the parent to
 * the next.
 *
 * @code
 * struct note_row {
 *   struct props {
 *     notes::note item;
 *   };
 *
 *   state<bool> editing;
 *
 *   element view(props const &given) {
 *     return li(given.item.text) | on_click([this] { editing = true; });
 *   }
 * };
 * @endcode
 *
 * `given` lives as long as what that view made: its handlers and their tasks may keep a reference to it. The
 * component, and the components it places, may keep a reference to `given` until its next view that returns, as a view
 * that throws keeps none; and mounted() to the props of the first view, until it ends. The props it gives a component
 * of its view may refer to it, its `this` or its `given`: they stay as long as those props. One with no props, a
 * Component, is placed with component<C>().
 */
template <class C>
concept ComponentWithProps = requires { typename C::props; } && requires(C &component, typename C::props const &given) {
  { component.view(given) } -> std::same_as<element>;
};

} // namespace quoll::ui

#endif // QUOLL_UI_COMPONENT_WITH_PROPS_HPP
