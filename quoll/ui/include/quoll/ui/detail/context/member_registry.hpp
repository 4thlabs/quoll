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

#ifndef QUOLL_UI_DETAIL_CONTEXT_MEMBER_REGISTRY_HPP
#define QUOLL_UI_DETAIL_CONTEXT_MEMBER_REGISTRY_HPP

#include <quoll/ui/detail/context/context_finder.hpp>
#include <quoll/ui/detail/context/member_join.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/state_link.hpp>

#include <cstdint>
#include <vector>

namespace quoll::ui::detail {

// The members of a context, in the order they were made, as each joined it: what was found by walking its members.
//
// Thread safety: none, as its app.
class member_registry {
public:
  void add(registered_member member) { members_.push_back(member); }

  // The member that joined by `join` leaves, as it goes before its context.
  void remove(void const *join) noexcept;

  // Each member that tells of its changes tells `to` from now on.
  void link(state_link to) noexcept;

  // After a view, or before the first with `start.first`: each effect whose dependency changed starts. One that cannot
  // start leaves the others to start all the same, and what it threw is reported, each failure, or, with no reporter,
  // the first thrown once they have.
  void run_effects(effect_starter const &start);

  // As its component's view is about to run, what its members see of the contexts they use; kept once the view shows;
  // and whether one read changed since.
  void mark() noexcept;
  void commit() noexcept;
  [[nodiscard]] bool contexts_changed() const noexcept;

private:
  std::vector<registered_member> members_;
};

// Makes what is made in its scope the context whose members join `into`, finding through `finder`, as Quoll does as it
// makes a context; the one made before, a context making another, again once it goes.
class making_scope {
public:
  // What is made in its scope is a component written as a struct, in the storage [begin, end), its props in `props`.
  making_scope(
      member_registry &into, context_finder const &finder, void const *begin, void const *end, void const *const *props
  ) noexcept;

  making_scope(making_scope const &) = delete;
  making_scope(making_scope &&) = delete;
  making_scope &operator=(making_scope const &) = delete;
  making_scope &operator=(making_scope &&) = delete;

  ~making_scope();

private:
  making_context making_;
  making_context *outer_;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CONTEXT_MEMBER_REGISTRY_HPP
