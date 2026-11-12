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

#ifndef QUOLL_UI_DETAIL_CONTEXT_MEMBER_HPP
#define QUOLL_UI_DETAIL_CONTEXT_MEMBER_HPP

#include <quoll/ui/detail/context_entry.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace quoll::ui::detail {

// What the app reaches of a shared<T> that its component does not: marking what it sees of its context as its view
// runs, keeping that once the view shows, and whether the context changed since.
struct context_access {
  template <class M>
  static void mark(M &member) noexcept {
    member.mark();
  }

  template <class M>
  static void commit(M &member) noexcept {
    member.commit();
  }

  template <class M>
  [[nodiscard]] static bool changed(M const &member) noexcept {
    return member.changed();
  }
};

// What finds a context of its app, as a shared<T> does, and says how many times its states changed.
template <class T>
concept ContextMember = requires(T &member, T const &held, context_finder const &finder) {
  typename T::element_type;
  member.bind(finder);
  { held.context_version() } noexcept -> std::same_as<std::uint64_t>;
};

// What provides a context to the components of its component's view, as a provided<T> does: the context, and its
// own uses of others, which it finds as its component's would.
template <class T>
concept ProvidedMember = requires(T &member, context_finder const &finder) {
  typename T::provided_type;
  { member.entry() } noexcept -> std::same_as<context_entry &>;
  member.bind(finder);
};

// What finds a context, or provides one that does: what a table walks.
template <class T>
struct is_context_user : std::bool_constant<ContextMember<T> || ProvidedMember<T>> {};

// ContextMember, as a trait for any_member().
template <class T>
struct is_context_member : std::bool_constant<ContextMember<T>> {};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CONTEXT_MEMBER_HPP
