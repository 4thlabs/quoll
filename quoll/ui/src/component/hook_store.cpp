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

#include <quoll/ui/detail/component/hook_store.hpp>

#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace quoll::ui::detail {

namespace {

// Per thread: an app runs on one, and the view of a function component runs there, alone at a time.
thread_local hook_store *current_store = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Two places of a source: the same file, line and column.
[[nodiscard]] bool same_place(std::source_location const &left, std::source_location const &right) noexcept {
  return left.line() == right.line() && left.column() == right.column() &&
         (left.file_name() == right.file_name() || std::strcmp(left.file_name(), right.file_name()) == 0);
}

} // namespace

hook_store::~hook_store() {
  // The last made first, as the members of a struct go.
  while (!hooks_.empty()) {
    hooks_.pop_back();
  }
}

hook_store &hook_store::current() {
  if (current_store == nullptr) {
    throw std::logic_error{"quoll::ui: a use_ hook called outside the view of a function component"};
  }

  return *current_store;
}

void hook_store::link(state_link to) noexcept { link_ = to; }

void hook_store::changed_in(void *self) { tell(static_cast<hook_store const *>(self)->link_); }

void hook_store::begin_view() noexcept {
  calls_.clear();
  cursor_ = 0;

  for (auto &hook : hooks_) {
    if (hook.ops->unwritten != nullptr) {
      hook.ops->unwritten(hook.object.get());
    }
  }
}

void hook_store::end_view() noexcept { viewed_ = true; }

void hook_store::effects(effect_starter const &start) {
  std::exception_ptr first;

  // As the effects of a struct: one that cannot start leaves the others to start all the same.
  for (auto &hook : hooks_) {
    if (hook.ops->after_view == nullptr) {
      continue;
    }

    try {
      hook.ops->after_view(hook.object.get(), start);
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

void hook_store::mark() noexcept {
  for (auto &hook : hooks_) {
    if (hook.ops->mark != nullptr) {
      hook.ops->mark(hook.object.get());
    }
  }
}

void hook_store::commit() noexcept {
  for (auto &hook : hooks_) {
    if (hook.ops->commit != nullptr) {
      hook.ops->commit(hook.object.get());
    }
  }
}

bool hook_store::changed() const noexcept {
  for (auto const &hook : hooks_) {
    if (hook.ops->changed != nullptr && hook.ops->changed(hook.object.get())) {
      return true;
    }
  }

  return false;
}

context_entry *hook_store::provides(async::detail::type_id type) const noexcept {
  for (auto const &hook : hooks_) {
    if (hook.ops->provides != nullptr) {
      if (auto *const found = hook.ops->provides(hook.object.get(), type); found != nullptr) {
        return found;
      }
    }
  }

  return nullptr;
}

std::uint32_t hook_store::next_call(std::source_location const &where) {
  for (auto &[place, count] : calls_) {
    if (same_place(place, where)) {
      return count++;
    }
  }

  calls_.emplace_back(where, 1);
  return 0;
}

void *hook_store::find_made(std::source_location const &where, std::uint32_t call, async::detail::type_id type) {
  auto const matches = [&](hook const &held) { return held.call == call && same_place(held.where, where); };
  auto const at = [&](std::size_t index) -> void * {
    if (hooks_[index].type != type) {
      throw std::logic_error{"quoll::ui: a use_ hook of another type where one was called before"};
    }

    cursor_ = index + 1;
    return hooks_[index].object.get();
  };

  // The next one first: a view calls them in the order it made them, but for a branch.
  if (cursor_ < hooks_.size() && matches(hooks_[cursor_])) {
    return at(cursor_);
  }

  for (std::size_t index = 0; index < hooks_.size(); ++index) {
    if (matches(hooks_[index])) {
      return at(index);
    }
  }

  return nullptr;
}

void hook_store::add(
    std::source_location const &where, std::uint32_t call, async::detail::type_id type, held_component object,
    hook_ops const &ops
) {
  auto *const made = object.get();

  hooks_.push_back({.where = where, .call = call, .type = type, .object = std::move(object), .ops = &ops});
  cursor_ = hooks_.size();

  // Linked at once, to the store, which tells the app: a change in the view that made it asks for another view.
  if (ops.link != nullptr) {
    ops.link(made, {this, &hook_store::changed_in});
  }
}

hook_scope::hook_scope(hook_store &store) noexcept : store_(store), previous_(std::exchange(current_store, &store)) {
  store_.begin_view();
}

hook_scope::~hook_scope() {
  store_.end_view();
  current_store = previous_;
}

held_component make_hook_store() { return make_held<hook_store>(); }

void link_hook_store(void *store, state_link to) { static_cast<hook_store *>(store)->link(to); }

void bind_hook_store(void *store, context_finder const &finder) { static_cast<hook_store *>(store)->bind(finder); }

void mark_hook_store(void *store) { static_cast<hook_store *>(store)->mark(); }

void commit_hook_store(void *store) { static_cast<hook_store *>(store)->commit(); }

bool hook_store_changed(void *store) { return static_cast<hook_store *>(store)->changed(); }

void hook_store_effects(void *store, effect_starter const &start) { static_cast<hook_store *>(store)->effects(start); }

bool same_without_hooks_props(void const * /*left*/, void const * /*right*/) noexcept { return true; }

context_entry *hook_store_provides(void *store, async::detail::type_id type) {
  return static_cast<hook_store *>(store)->provides(type);
}

} // namespace quoll::ui::detail
