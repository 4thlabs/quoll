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

#ifndef QUOLL_UI_PROVIDE_HPP
#define QUOLL_UI_PROVIDE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/component_with_props.hpp>
#include <quoll/ui/context_spec.hpp>
#include <quoll/ui/detail/bind_contexts.hpp>
#include <quoll/ui/detail/context_entry.hpp>
#include <quoll/ui/detail/held_component.hpp>
#include <quoll/ui/detail/link_states.hpp>
#include <quoll/ui/detail/run_effects.hpp>
#include <quoll/ui/mounted_component.hpp>
#include <quoll/ui/unmounted_component.hpp>

#include <type_traits>

namespace quoll::ui {

struct router;

namespace detail {

template <class T>
inline constexpr context_table context_table_of{
    .type = async::detail::type_id_of<T>(),
    .make = &make_held<T>,
    .link = [](void *context, state_link to) { link_states(*static_cast<T *>(context), to); },
    .effects = effects_of<T>(),
    .finds = finds_of<T>(),
    .bind = binds_of<T>(),
    .contexts_version = contexts_version_of<T>(),
};

} // namespace detail

/**
 * @brief The context of type T, for an app's options: the app makes it as it is made, and holds it as long as it
 * lives, for each component using a `shared<T>`.
 *
 * @code
 * mount<dashboard_page>("#app", {.contexts = {provide<catalog>()}});
 * @endcode
 *
 * T is an aggregate, as a component, with no view, mounted() nor unmounted(): its states, its effects, which start
 * after each view of the app and do what a component's mounted() would, and what its components run on it, through
 * shared<T>::run().
 */
template <class T>
[[nodiscard]] constexpr context_spec provide() noexcept {
  static_assert(std::is_aggregate_v<T> && !std::is_const_v<T>,
                "quoll::ui: a context is an aggregate, as a component: public members and no constructor");
  static_assert(!std::is_same_v<T, router>, "quoll::ui: an app's router is made by routing(), with its routes");
  static_assert(
      !MountedComponent<T> && !UnmountedComponent<T> && !ComponentWithProps<T> &&
          !requires(T &context) { context.view(); },
      "quoll::ui: a context has no view, mounted() nor unmounted(), which would never run: its effects do "
      "what a component's mounted() would");

  return context_spec{detail::context_table_of<T>};
}

} // namespace quoll::ui

#endif // QUOLL_UI_PROVIDE_HPP
