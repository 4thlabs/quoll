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

#ifndef QUOLL_UI_DETAIL_COMPONENT_STRUCT_TABLE_HPP
#define QUOLL_UI_DETAIL_COMPONENT_STRUCT_TABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/detail/component/component_table.hpp>
#include <quoll/ui/detail/component/hook_store.hpp>
#include <quoll/ui/detail/component/hooks_table.hpp>
#include <quoll/ui/detail/context/member_registry.hpp>
#include <quoll/ui/view/element.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <new>

namespace quoll::ui::detail {

// The props of a component written as a struct: its `struct props`, none without one.
template <class C>
struct struct_props {
  using type = void;
};

template <class C>
  requires requires { typename C::props; }
struct struct_props<C> {
  using type = typename C::props;
};

template <class C>
using struct_props_t = typename struct_props<C>::type;

// What the app holds of a component written as a struct: the struct, made in its storage once the contexts it uses can
// be found, its members as they joined it, where it reads its props, and the hooks its view may still call, as
// use_context(). Neither copied nor moved: its members refer to it.
//
// Thread safety: none, as its app.
template <class C>
class struct_box {
public:
  explicit struct_box(void const *props) noexcept : props_(props) {}

  struct_box(struct_box const &) = delete;
  struct_box(struct_box &&) = delete;
  struct_box &operator=(struct_box const &) = delete;
  struct_box &operator=(struct_box &&) = delete;

  // The struct first, whose members leave what they joined as they go.
  ~struct_box() {
    if (made_) {
      std::destroy_at(object());
    }
  }

  // Made, its members joining it and finding the contexts they use through `finder`; one missing throws, and nothing
  // of it is left.
  void make(context_finder const &finder) {
    hooks_.bind(finder);

    making_scope const scope{members_, finder,
                             storage_, storage_ + sizeof(C),
                             &props_,  async::detail::type_id_of<struct_props_t<C>>()};

    std::construct_at(reinterpret_cast<C *>(storage_));
    made_ = true;
  }

  [[nodiscard]] C *object() noexcept { return std::launder(reinterpret_cast<C *>(storage_)); }

  hook_store hooks_;
  member_registry members_;
  // The props of its last view, which its base's `given` reads.
  void const *props_;

private:
  alignas(C) std::byte storage_[sizeof(C)];
  bool made_ = false;
};

template <class C>
[[nodiscard]] held_component make_struct(void const *props) {
  auto box = std::make_unique<struct_box<C>>(props);

  return held_component{box.release(), [](void *held) noexcept {
                          std::unique_ptr<struct_box<C>>{static_cast<struct_box<C> *>(held)}.reset();
                        }};
}

template <class C>
[[nodiscard]] struct_box<C> &box_of(void *held) noexcept {
  return *static_cast<struct_box<C> *>(held);
}

template <class C>
void link_struct(void *held, state_link to) {
  link_hook_store(&box_of<C>(held).hooks_, to);
  box_of<C>(held).members_.link(to);
}

template <class C>
context_entry *struct_provides(void *held, async::detail::type_id type) {
  return hook_store_provides(&box_of<C>(held).hooks_, type);
}

template <class C>
void bind_struct(void *held, context_finder const &finder) {
  box_of<C>(held).make(finder);
}

template <class C>
void mark_struct(void *held) {
  auto &box = box_of<C>(held);

  mark_hook_store(&box.hooks_);
  box.members_.mark();
}

template <class C>
void commit_struct(void *held) {
  auto &box = box_of<C>(held);

  commit_hook_store(&box.hooks_);
  box.members_.commit();
}

template <class C>
bool struct_changed(void *held) {
  auto &box = box_of<C>(held);

  return hook_store_changed(&box.hooks_) || box.members_.contexts_changed();
}

template <class C>
void struct_effects(void *held, effect_starter const &start) {
  auto &box = box_of<C>(held);
  std::exception_ptr first;

  // Those of its view's hooks, then its members': one that cannot start leaves the others to start all the same.
  auto const run = [&](auto const &start_them) {
    try {
      start_them();
    } catch (...) {
      if (!first) {
        first = std::current_exception();
      }
    }
  };

  run([&] { hook_store_effects(&box.hooks_, start); });
  run([&] { box.members_.run_effects(start); });

  if (first) {
    std::rethrow_exception(first);
  }
}

template <class C>
element view_struct(void *held, void const *props) {
  auto &box = box_of<C>(held);
  // The props of its last view stay, should this one throw: those it is given go then, and its handlers still read.
  auto const *const before = box.props_;

  box.props_ = props;

  try {
    hook_scope const scope{box.hooks_};

    // A struct with no view is refused by its tag, with a message of its own.
    if constexpr (requires { box.object()->view(); }) {
      return box.object()->view();
    } else {
      return {};
    }
  } catch (...) {
    box.props_ = before;
    throw;
  }
}

// One for each struct a component is written as: what tells two components apart.
template <class C>
inline constexpr component_table struct_table_of{
    .type = async::detail::type_id_of<C>(),
    .make = &make_struct<C>,
    .link = &link_struct<C>,
    .provides = &struct_provides<C>,
    .bind = &bind_struct<C>,
    .mark_contexts = &mark_struct<C>,
    .commit_contexts = &commit_struct<C>,
    .contexts_changed = &struct_changed<C>,
    .effects = &struct_effects<C>,
    .view = &view_struct<C>,
    .same_props = same_hooks_props_of<struct_props_t<C>>(),
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_STRUCT_TABLE_HPP
