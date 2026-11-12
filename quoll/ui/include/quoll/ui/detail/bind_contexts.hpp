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

#ifndef QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP
#define QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP

#include <quoll/ui/detail/context_entry.hpp>
#include <quoll/ui/detail/context_member.hpp>
#include <quoll/ui/detail/for_each_member.hpp>

#include <cstdint>
#include <type_traits>

namespace quoll::ui::detail {

// What a table holds to find the contexts the members of `C` use: none for one that uses none.
template <class C>
[[nodiscard]] constexpr auto binds_of() noexcept -> void (*)(void *, context_finder const &) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object, context_finder const &finder) {
      for_each_member(*static_cast<C *>(object), [&finder](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          member.bind(finder);
        }
      });
    };
  } else {
    return nullptr;
  }
}

// What a table holds to count the changes of the contexts the members of `C` use, which a view not run again
// compares: none for one that uses none.
template <class C>
[[nodiscard]] constexpr auto contexts_version_of() noexcept -> std::uint64_t (*)(void *) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object) {
      std::uint64_t total = 0;

      for_each_member(*static_cast<C *>(object), [&total](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          total += member.version();
        }
      });

      return total;
    };
  } else {
    return nullptr;
  }
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP
