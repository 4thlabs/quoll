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

#ifndef QUOLL_UI_DETAIL_CONTEXT_MEMBER_JOIN_HPP
#define QUOLL_UI_DETAIL_CONTEXT_MEMBER_JOIN_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/detail/data/state_link.hpp>

namespace quoll::ui::detail {

struct context_finder;
struct effect_starter;
class member_registry;

// A member of a context that tells the app of its changes, runs after views, or uses a context: what the app reaches
// of it, none for what it does not do, and what it left by, its join.
struct registered_member {
  void *object = nullptr;
  void const *join = nullptr;
  void (*link)(void *member, state_link to) noexcept = nullptr;
  void (*after_view)(void *member, effect_starter const &start) = nullptr;
  // For one using a context: what its component's view sees of it, kept once the view shows, and whether it changed
  // since, once read.
  void (*mark)(void *member) noexcept = nullptr;
  void (*commit)(void *member) noexcept = nullptr;
  bool (*changed)(void const *member) noexcept = nullptr;
};

// What a member using a context gives its component, as shared<T> does in its header: none for the others.
template <class M>
struct joined_context {};

// While Quoll makes a context: where its members join, the storage it is made in, and what finds the contexts it uses.
struct making_context {
  member_registry *into = nullptr;
  context_finder const *finder = nullptr;
  void const *begin = nullptr;
  void const *end = nullptr;
  // Where a component written as a struct finds its props, as its base reads them, and their type: none for a
  // context.
  void const *const *props = nullptr;
  async::detail::type_id props_type = nullptr;

  // Whether `member` is in the context's storage, rather than a value its making uses for a moment.
  [[nodiscard]] bool holds(void const *member) const noexcept;
};

// The context being made, none outside the making of one.
[[nodiscard]] making_context *current_making() noexcept;

// Where the props of the component written as a struct being made are kept, none outside its making.
[[nodiscard]] void const *const *making_props() noexcept;

// Where the props of the struct being made are kept, for its member `member` reading them as props of `type`.
// @throws std::logic_error outside the making of a struct, for what is no member of it, or for props of another type.
[[nodiscard]] void const *const *making_props_of(async::detail::type_id type, void const *member);

// `member` joins `into`, and leaves it by its join, as it goes before its context.
void join(member_registry &into, registered_member member);
void leave(member_registry &into, void const *join) noexcept;

// Joins `member`, whose join is `by`, to the context being made, if it is in its storage: Quoll's states, effects,
// forms and shared<T> do as they are made, so that nothing walks the context's members. A shared<T> finds its context
// there, and throws when the app has none: the context is then refused before its constructor ends, its destructor
// never run. What it joined, none outside the making of a context.
template <class M>
[[nodiscard]] member_registry *join_making(M &member, void const *by) {
  auto *const making = current_making();

  if (making == nullptr || !making->holds(&member)) {
    return nullptr;
  }

  registered_member joined{.object = &member, .join = by};

  if constexpr (requires(state_link to) { member.link(to); }) {
    joined.link = [](void *object, state_link to) noexcept { static_cast<M *>(object)->link(to); };
  }

  if constexpr (requires(effect_starter const &start) { member.after_view(start); }) {
    joined.after_view = [](void *object, effect_starter const &start) { static_cast<M *>(object)->after_view(start); };
  }

  if constexpr (requires { &joined_context<M>::changed; }) {
    member.bind(*making->finder);
    joined.mark = &joined_context<M>::mark;
    joined.commit = &joined_context<M>::commit;
    joined.changed = &joined_context<M>::changed;
  }

  join(*making->into, joined);
  return making->into;
}

// The last member of a state, an effect, a form or a shared<T>: joins it to the context being made, as it is made, and
// leaves as it goes, so that one in an optional, a variant or a vector of the context, which may go first, leaves no
// trace. Copied or assigned, it keeps what it joined: a copy of its member joins as it is made.
template <class M>
class member_join {
public:
  // A move of its member, noexcept, joins too: a failure to note it terminates, as nothing else could say it.
  explicit member_join(M &member) : into_(join_making(member, this)) {}

  member_join(member_join const &) = delete;
  member_join(member_join &&) = delete;

  // NOLINTNEXTLINE(cert-oop54-cpp): its member's assignment keeps what each joined.
  member_join &operator=(member_join const & /*other*/) noexcept { return *this; }

  member_join &operator=(member_join && /*other*/) noexcept { return *this; }

  ~member_join() {
    if (into_ != nullptr) {
      leave(*into_, this);
    }
  }

private:
  member_registry *into_;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CONTEXT_MEMBER_JOIN_HPP
