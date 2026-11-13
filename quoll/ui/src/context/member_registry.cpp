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

#include <quoll/ui/detail/context/member_registry.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <iterator>
#include <vector>

namespace quoll::ui::detail {

namespace {

// The context being made on this thread: an app and its contexts are made on one.
thread_local making_context *current = nullptr;

} // namespace

void member_registry::link(state_link to) noexcept {
  for (auto const &member : members_) {
    if (member.link != nullptr) {
      member.link(member.object, to);
    }
  }
}

void member_registry::run_effects(effect_starter const &start) {
  std::exception_ptr first;

  for (auto const &member : members_) {
    if (member.after_view == nullptr) {
      continue;
    }

    try {
      member.after_view(member.object, start);
    } catch (...) {
      if (start.report != nullptr) {
        start.report(start.owner, std::current_exception());
      } else if (!first) {
        first = std::current_exception();
      }
    }
  }

  if (first) {
    std::rethrow_exception(first);
  }
}

void member_registry::mark() noexcept {
  for (auto const &member : members_) {
    if (member.mark != nullptr) {
      member.mark(member.object);
    }
  }
}

void member_registry::commit() noexcept {
  for (auto const &member : members_) {
    if (member.commit != nullptr) {
      member.commit(member.object);
    }
  }
}

bool member_registry::contexts_changed() const noexcept {
  return std::ranges::any_of(members_, [](registered_member const &member) {
    return member.changed != nullptr && member.changed(member.object);
  });
}

void member_registry::remove(void const *join) noexcept {
  // From the back: members leave in the reverse order of their joining.
  auto const found = std::ranges::find(members_.rbegin(), members_.rend(), join, &registered_member::join);

  if (found != members_.rend()) {
    members_.erase(std::next(found).base());
  }
}

void join(member_registry &into, registered_member member) { into.add(member); }

void leave(member_registry &into, void const *join) noexcept { into.remove(join); }

bool making_context::holds(void const *member) const noexcept {
  std::less<> const before;

  return begin != nullptr && !before(member, begin) && before(member, end);
}

making_context *current_making() noexcept { return current; }

making_scope::making_scope(
    member_registry &into, context_finder const &finder, void const *begin, void const *end, void const *const *props
) noexcept
    : making_{.into = &into, .finder = &finder, .begin = begin, .end = end, .props = props}, outer_(current) {
  current = &making_;
}

making_scope::~making_scope() { current = outer_; }

void const *const *making_props() noexcept { return current == nullptr ? nullptr : current->props; }

} // namespace quoll::ui::detail
