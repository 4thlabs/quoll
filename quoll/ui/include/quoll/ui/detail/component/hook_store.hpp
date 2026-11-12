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

#ifndef QUOLL_UI_DETAIL_COMPONENT_HOOK_STORE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_HOOK_STORE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/detail/component/committed_hook.hpp>
#include <quoll/ui/detail/component/held_component.hpp>
#include <quoll/ui/detail/context/context_entry.hpp>
#include <quoll/ui/detail/context/context_member.hpp>
#include <quoll/ui/detail/data/effect_member.hpp>
#include <quoll/ui/detail/data/effect_starter.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/state_link.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

// What a store does with a hook of one type, none of what that type does not do.
struct hook_ops {
  void (*destroy)(void *hook) noexcept;
  void (*link)(void *hook, state_link to) noexcept = nullptr;
  void (*after_view)(void *hook, effect_starter const &start) = nullptr;
  void (*mark)(void *hook) noexcept = nullptr;
  // Its component's view showed: what it saw of a context is kept, or what that view wrote takes effect.
  void (*commit)(void *hook) noexcept = nullptr;
  bool (*changed)(void const *hook) noexcept = nullptr;
  // The context it provides to the components of its view, of type `type`, none for another.
  context_entry *(*provides)(void *hook, async::detail::type_id type) noexcept = nullptr;
  // A view of its component starts: what one before it wrote and did not show goes.
  void (*unwritten)(void *hook) noexcept = nullptr;
};

// The entries of hook_ops for a hook of type H: functions rather than lambdas in a variable template, which GCC 13 may
// leave empty at run time.
template <class H>
void destroy_hook(void *hook) noexcept {
  std::default_delete<H>{}(static_cast<H *>(hook));
}

template <class H>
void link_hook(void *hook, state_link to) noexcept {
  static_cast<H *>(hook)->link(to);
}

template <class H>
void after_view_hook(void *hook, effect_starter const &start) {
  static_cast<H *>(hook)->after_view(start);
}

template <class H>
void mark_hook(void *hook) noexcept {
  context_access::mark(*static_cast<H *>(hook));
}

template <class H>
void commit_hook(void *hook) noexcept {
  context_access::commit(*static_cast<H *>(hook));
}

template <class H>
bool hook_changed(void const *hook) noexcept {
  return context_access::changed(*static_cast<H const *>(hook));
}

template <class H>
context_entry *hook_provides(void *hook, async::detail::type_id type) noexcept {
  return type == async::detail::type_id_of<typename H::provided_type>() ? &static_cast<H *>(hook)->entry() : nullptr;
}

template <class H>
void hook_committed(void *hook) noexcept {
  static_cast<H *>(hook)->committed();
}

template <class H>
void hook_unwritten(void *hook) noexcept {
  static_cast<H *>(hook)->unwritten();
}

template <class H>
[[nodiscard]] consteval hook_ops make_hook_ops() {
  static_assert(
      !((ContextMember<H> || ProvidedMember<H>) && CommittedHook<H>),
      "quoll::ui: a hook reads a context or holds what its view writes, not both: its one commit entry does one"
  );

  hook_ops ops{.destroy = &destroy_hook<H>};

  if constexpr (LinkedMember<H>) {
    ops.link = &link_hook<H>;
  }

  if constexpr (EffectMember<H>) {
    ops.after_view = &after_view_hook<H>;
  }

  if constexpr (ContextMember<H> || ProvidedMember<H>) {
    ops.mark = &mark_hook<H>;
    ops.commit = &commit_hook<H>;
    ops.changed = &hook_changed<H>;
  } else if constexpr (CommittedHook<H>) {
    ops.unwritten = &hook_unwritten<H>;
    ops.commit = &hook_committed<H>;
  }

  if constexpr (ProvidedMember<H>) {
    ops.provides = &hook_provides<H>;
  }

  return ops;
}

template <class H>
inline constexpr hook_ops hook_ops_of = make_hook_ops<H>();

// The hooks of a function component: what its use_ calls made, each found again by where it is called, and how many
// times that place was called before in the same view, so that a branch not taken shifts nothing and a function calling
// hooks may be called twice. A hook its view does not call keeps what it holds. Neither copied nor moved: its hooks are
// referred to by the view's handlers and the component's tasks, which it outlives, as a struct component's members.
//
// Thread safety: none, as its app.
class hook_store {
public:
  hook_store() = default;

  hook_store(hook_store const &) = delete;
  hook_store(hook_store &&) = delete;
  hook_store &operator=(hook_store const &) = delete;
  hook_store &operator=(hook_store &&) = delete;

  ~hook_store();

  // The store of the function component whose view runs on this thread.
  // Throws std::logic_error outside one, as a use_ call from a struct's view or a handler.
  [[nodiscard]] static hook_store &current();

  // The hook of type H called at `where`, made by `make`, a function giving a std::unique_ptr<H>, the first time;
  // `made` set when it was. Throws std::logic_error for another type there, which a hook of that place made before.
  template <class H, class Make>
  [[nodiscard]] H &find(std::source_location const &where, Make make, bool &made) {
    auto const call = next_call(where);

    if (auto *const found = find_made(where, call, async::detail::type_id_of<H>()); found != nullptr) {
      made = false;
      return *static_cast<H *>(found);
    }

    auto made_hook = make();
    auto &hook = *made_hook;

    // Owned at once, so that a lack of memory adding it destroys it.
    add(where, call, async::detail::type_id_of<H>(), held_component{made_hook.release(), hook_ops_of<H>.destroy},
        hook_ops_of<H>);
    made = true;
    return hook;
  }

  // Where its contexts are found, as its component was made: the component that placed it held weakly, as an
  // instance holds its parent, so that a child and its parent hold no cycle.
  [[nodiscard]] context_finder finder() const {
    auto found = finder_;

    found.from = from_.lock();
    return found;
  }

  void bind(context_finder const &finder) {
    finder_ = finder;
    from_ = finder.from;
    finder_.from.reset();
  }

  // Whether its component's first view is under way, or none has run: a hook made then says what it is before the
  // view reads it, as a struct's members do before their first view.
  [[nodiscard]] bool first_view() const noexcept { return !viewed_; }

  void link(state_link to) noexcept;

  // As its view starts: no place called yet.
  void begin_view() noexcept;

  // As its view ends, returned or thrown: the hooks it made are linked to the app, so that what they did while the view
  // ran, as a target set loading, asks for no other view.
  void end_view() noexcept;

  void effects(effect_starter const &start);
  void mark() noexcept;
  // Its component's view showed: what it saw of its contexts is kept, and what the view wrote takes effect.
  void commit() noexcept;
  [[nodiscard]] bool changed() const noexcept;
  [[nodiscard]] context_entry *provides(async::detail::type_id type) const noexcept;

private:
  struct hook {
    std::source_location where;
    std::uint32_t call;
    async::detail::type_id type;
    held_component object;
    hook_ops const *ops;
    bool linked;
  };

  [[nodiscard]] std::uint32_t next_call(std::source_location const &where);
  [[nodiscard]] void *find_made(std::source_location const &where, std::uint32_t call, async::detail::type_id type);
  void
  add(std::source_location const &where, std::uint32_t call, async::detail::type_id type, held_component object,
      hook_ops const &ops);

  std::vector<hook> hooks_;
  // Where the next hook is likely to be: the one after the last found, as a view calls them in the same order.
  std::size_t cursor_ = 0;
  // How many times each place was called in the view under way.
  std::vector<std::pair<std::source_location, std::uint32_t>> calls_;
  context_finder finder_{};
  std::weak_ptr<void const> from_{};
  state_link link_{};
  bool viewed_ = false;
};

// Makes `store` the current one while it lives, its view under way: the view of a function component.
class hook_scope {
public:
  explicit hook_scope(hook_store &store) noexcept;

  hook_scope(hook_scope const &) = delete;
  hook_scope(hook_scope &&) = delete;
  hook_scope &operator=(hook_scope const &) = delete;
  hook_scope &operator=(hook_scope &&) = delete;

  ~hook_scope();

private:
  hook_store &store_;
  hook_store *previous_;
};

// The entries of a function component's table that do not depend on the function: compiled once.
[[nodiscard]] held_component make_hook_store();
void link_hook_store(void *store, state_link to);
void bind_hook_store(void *store, context_finder const &finder);
void mark_hook_store(void *store);
void commit_hook_store(void *store);
[[nodiscard]] bool hook_store_changed(void *store);
void hook_store_effects(void *store, effect_starter const &start);
[[nodiscard]] bool same_without_hooks_props(void const *left, void const *right) noexcept;
[[nodiscard]] context_entry *hook_store_provides(void *store, async::detail::type_id type);

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_HOOK_STORE_HPP
