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

#include <quoll/ui/detail/expand.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace quoll::ui::detail {

namespace {

using async::detail::type_id;

// The components of a view, matched against those it has now: by type and key, those without a key in their order, as
// their rank among those of their type. Sorted once: a few allocations for a view, rather than some for each place.
class matching {
public:
  explicit matching(std::vector<std::shared_ptr<instance>> const &shown) : shown_(&shown) {
    if (shown.empty()) {
      return;
    }

    taken_.assign(shown.size(), false);
    cursors_.assign(shown.size(), 0);
    places_.reserve(shown.size());

    for (std::size_t index = 0; index < shown.size(); ++index) {
      places_.push_back({shown[index]->table().type, &shown[index]->key(), index});
    }

    std::ranges::sort(places_, before);
  }

  [[nodiscard]] std::shared_ptr<instance> take(type_id type, std::string const &key) {
    auto const first = std::ranges::lower_bound(places_, place{type, &key, 0}, before);

    if (first == places_.end() || !first->is(type, key)) {
      return nullptr;
    }

    auto const group = static_cast<std::size_t>(first - places_.begin());
    auto const at = group + cursors_[group];

    if (at >= places_.size() || !places_[at].is(type, key)) {
      return nullptr;
    }

    ++cursors_[group];
    taken_[places_[at].index] = true;
    return (*shown_)[places_[at].index];
  }

  // A keyed component placed by this view: two of one type may not share a key there.
  void placed(type_id type, std::string const &key) { keyed_.emplace_back(type, &key); }

  void check_keys() {
    if (keyed_.size() < 2) {
      return;
    }

    std::ranges::sort(keyed_, [](auto const &left, auto const &right) {
      return less(left.first, *left.second, right.first, *right.second);
    });

    auto const same = [](auto const &left, auto const &right) {
      return left.first == right.first && *left.second == *right.second;
    };

    if (std::ranges::adjacent_find(keyed_, same) != keyed_.end()) {
      throw std::logic_error{"quoll::ui: two components of one type have the same key in one view"};
    }
  }

  // The rank of the next component of `type` without a key, among those of this view.
  [[nodiscard]] std::size_t rank(type_id type) {
    for (auto &[seen, count] : ranks_) {
      if (seen == type) {
        return count++;
      }
    }

    ranks_.emplace_back(type, 1);
    return 0;
  }

  void leave_the_rest(expansion &context) const {
    for (std::size_t index = 0; index < taken_.size(); ++index) {
      if (!taken_[index]) {
        context.leaving.push_back((*shown_)[index]);
      }
    }
  }

private:
  struct place {
    type_id type;
    std::string const *key;
    std::size_t index;

    [[nodiscard]] bool is(type_id other, std::string const &other_key) const noexcept {
      return type == other && *key == other_key;
    }
  };

  // By type, then by key.
  [[nodiscard]] static bool less(type_id left_type, std::string const &left_key, type_id right_type,
                                 std::string const &right_key) noexcept {
    if (left_type != right_type) {
      return std::less<>{}(left_type, right_type);
    }

    return left_key < right_key;
  }

  [[nodiscard]] static bool before(place const &left, place const &right) noexcept {
    if (!left.is(right.type, *right.key)) {
      return less(left.type, *left.key, right.type, *right.key);
    }

    return left.index < right.index;
  }

  std::vector<std::shared_ptr<instance>> const *shown_;
  std::vector<place> places_;
  // For the first place of each type and key, how many of them are taken.
  std::vector<std::size_t> cursors_;
  std::vector<bool> taken_;
  std::vector<std::pair<type_id, std::size_t>> ranks_;
  std::vector<std::pair<type_id, std::string const *>> keyed_;
};

// The component whose view is walked, none for the app's own, and the props it was given for it.
struct made_by {
  std::shared_ptr<instance> const &owner;
  std::shared_ptr<void const> const &props;
};

// The props of a component, holding the component that placed it and the props of that view: a pointer into either
// stays good as long as its own props.
struct chained_props {
  // Before its own, which may refer to them: destroyed after them. The component that placed it, its parent, and the
  // props of that view: they live as long as the props do, whatever holds those, and in whatever order it lets go.
  std::shared_ptr<instance> placed_in;
  std::shared_ptr<void const> placed_by;
  std::shared_ptr<void const> own;
};

void walk(element &value, matching &shown, std::vector<std::shared_ptr<instance>> &now, made_by by, expansion &context);

void walk_children(std::vector<element> &children, matching &shown, std::vector<std::shared_ptr<instance>> &now,
                   made_by by, expansion &context) {
  auto fragments = false;

  for (auto &child : children) {
    walk(child, shown, now, by, context);
    fragments = fragments || child.type == element::kind::fragment;
  }

  if (!fragments) {
    return;
  }

  std::vector<element> flat;

  flat.reserve(children.size());

  for (auto &child : children) {
    append_child(flat, std::move(child));
  }

  children = std::move(flat);
}

void check_undecorated(element const &value) {
  if (!value.tag.empty() || !value.content.empty() || !value.classes.empty() || !value.attributes.empty() ||
      !value.events.empty() || value.value || !value.children.empty()) {
    throw std::logic_error{"quoll::ui: a component is decorated in its own view, but for its key"};
  }
}

// A key as a part of a longer one: its length first.
[[nodiscard]] std::string segment(std::string const &key) { return std::to_string(key.size()) + ':' + key; }

// Where the nodes of a component's view go among its parent's: its type, and its key, or its rank among those of its
// type without one, starting with a 0 character, which no key starts with. Positions start with a 0 alone, as the
// reconciler's own.
[[nodiscard]] std::string scope_of(type_id type, std::string const &key, matching &shown) {
  auto scope =
      std::string{'\0', key.empty() ? 'r' : 'k'} + std::to_string(reinterpret_cast<std::uintptr_t>(type)) + ':';

  scope += key.empty() ? std::to_string(shown.rank(type)) + ':' : segment(key);
  return scope;
}

// Each node of a component's view gets its scope, ahead of its own key or of its position in a fragment, so that the
// nodes move with the component. Each part says where it ends, a key by its length, so that two never read the same.
void scope_keys(element &view, std::string const &scope) {
  if (view.type != element::kind::fragment) {
    view.key = view.key.empty() ? scope : scope + segment(view.key);
    return;
  }

  for (std::size_t index = 0; index < view.children.size(); ++index) {
    auto &child = view.children[index];

    scope_keys(child, child.key.empty() ? scope + '\0' + std::to_string(index) + ':' : scope);
  }
}

// Runs the view of `kept` with `props`, and records it for commit(): a view that threw records nothing.
[[nodiscard]] element run_view(component_spec const &spec, std::shared_ptr<instance> const &kept,
                               std::shared_ptr<void const> const &props, made_by by, expansion &context) {
  auto const version = kept->version();

  // Room for its record, made once its view has returned: recording it then cannot fail.
  if (context.views.size() == context.views.capacity()) {
    context.views.reserve(std::max<std::size_t>(4, context.views.capacity() * 2));
  }

  auto view = [&] {
    // Its callbacks call this component back, in its tasks, with these props.
    view_scope const scope{{.app = context.app, .held = {.owner = kept, .props = props}}};

    return spec.table->view(kept->component(), props.get());
  }();

  // Recorded at once, which cannot fail: then the view it keeps, copied, which may.
  context.views.push_back({.owner = kept,
                           .props = props,
                           .own = spec.props,
                           .placed_by = by.props,
                           .view = {},
                           .version = version,
                           .epoch = context.epoch});
  context.views.back().view = view;
  return view;
}

void walk_component(element &value, matching &shown, std::vector<std::shared_ptr<instance>> &now, made_by by,
                    expansion &context) {
  check_undecorated(value);

  auto const &spec = *value.component;
  auto const type = spec.table->type;
  auto const scope = scope_of(type, value.key, shown);
  auto kept = shown.take(type, value.key);
  auto const fresh = !kept;
  // Kept, its props made of the same as before, or giving the same view, and none of its states changed since: its
  // last view is given again, with the props it was made with, which its handlers may refer to.
  auto const same_sources = !fresh && kept->made_of(spec.props.get(), by.props.get());
  auto const *const unchanged = fresh ? nullptr : kept->unchanged_view(context.epoch);
  auto const given_again =
      unchanged != nullptr && (same_sources || (spec.table->same_props != nullptr &&
                                                spec.table->same_props(kept->own().get(), spec.props.get())));
  auto const props =
      given_again || same_sources ? kept->props()
      : by.owner && spec.props
          ? std::shared_ptr<void const>{std::make_shared<chained_props const>(chained_props{
                                            .placed_in = by.owner, .placed_by = by.props, .own = spec.props}),
                                        spec.props.get()}
          : spec.props;

  if (fresh) {
    kept = std::make_shared<instance>(*spec.table, props, value.key, context.app_stop);
  }

  now.push_back(kept);

  // Its own copy of the key, which outlives the element.
  if (!value.key.empty()) {
    shown.placed(type, kept->key());
  }

  auto view = given_again ? *unchanged : run_view(spec, kept, props, by, context);

  std::vector<std::shared_ptr<instance>> inner;
  matching inner_shown{kept->children()};

  walk(view, inner_shown, inner, {kept, props}, context);
  inner_shown.check_keys();
  inner_shown.leave_the_rest(context);
  context.children.emplace_back(kept, std::move(inner));

  // After those of its view, so that their mounted() run first, as the app's own runs after its components'.
  if (fresh) {
    context.made.push_back(kept);
  }

  scope_keys(view, scope);
  value = std::move(view);
}

void walk(element &value, matching &shown, std::vector<std::shared_ptr<instance>> &now, made_by by,
          expansion &context) {
  if (value.type == element::kind::component) {
    walk_component(value, shown, now, by, context);
    return;
  }

  for (auto &binding : value.events) {
    binding.held = {.owner = by.owner, .props = by.props};
  }

  walk_children(value.children, shown, now, by, context);
}

} // namespace

bool has_component(element const &value) noexcept {
  if (value.type == element::kind::component) {
    return true;
  }

  for (auto const &child : value.children) {
    if (has_component(child)) {
      return true;
    }
  }

  return false;
}

void expand(element &view, std::vector<std::shared_ptr<instance>> const &shown, expansion &context) {
  std::shared_ptr<instance> const app_own;
  std::shared_ptr<void const> const no_props;
  matching matched{shown};

  try {
    walk(view, matched, context.now, {app_own, no_props}, context);
    matched.check_keys();
    matched.leave_the_rest(context);
  } catch (...) {
    discard(context);
    throw;
  }
}

void commit(expansion &context, std::vector<std::shared_ptr<instance>> &shown) noexcept {
  // Shown now: what changes in them shows too.
  if (context.link.changed != nullptr) {
    for (auto const &made : context.made) {
      made->link(context.link);
    }
  }

  for (auto &record : context.views) {
    record.owner->viewed(std::move(record.props), std::move(record.own), std::move(record.placed_by),
                         std::move(record.view), record.version, record.epoch);
  }

  for (auto &[parent, children] : context.children) {
    parent->children() = std::move(children);
  }

  for (auto const &leaving : context.leaving) {
    leaving->unmount(context.sink);
  }

  shown = std::move(context.now);
  context.views.clear();
  context.children.clear();
  context.leaving.clear();
}

void discard(expansion &context) noexcept {
  // Their views returned with these, and may refer to them until they view again; their last view goes, made with
  // those before, so that the next render runs them again.
  for (auto &record : context.views) {
    record.owner->tried(std::move(record.props));
  }

  // Made, never shown nor linked: gone with these, as no parent holds its children before commit().
  context.made.clear();
  context.now.clear();
  context.views.clear();
  context.children.clear();
  context.leaving.clear();
}

} // namespace quoll::ui::detail
