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

#include <quoll/ui/detail/reconciler.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace quoll::ui::detail {

namespace {

[[nodiscard]] std::string key_of(element const &value, std::size_t position) {
  // A key from the application is text: it does not start with this character.
  return value.key.empty() ? std::string(1, '\0') + std::to_string(position) : value.key;
}

[[nodiscard]] std::unordered_map<std::string, std::size_t> index_by_key(std::vector<element> const &values) {
  std::unordered_map<std::string, std::size_t> by_key;

  for (std::size_t index = 0; index < values.size(); ++index) {
    [[maybe_unused]] auto const inserted = by_key.emplace(key_of(values[index], index), index).second;

    assert(inserted && "quoll::ui: two siblings have the same key");
  }

  return by_key;
}

[[nodiscard]] bool same_node(element const &before, element const &after) noexcept {
  return before.type == after.type && before.tag == after.tag;
}

// An element may run several functions on one event: a renderer listens to it once, or each would run several times.
[[nodiscard]] bool first_of_its_name(std::vector<event_binding> const &events, event_binding const &binding) {
  return &*std::ranges::find(events, binding.name, &event_binding::name) == &binding;
}

} // namespace

void reconciler::children(node_id parent, std::vector<element> &before, std::vector<element> &after) {
  auto const by_key = index_by_key(before);
  std::vector<std::optional<std::size_t>> kept(after.size());
  std::vector<bool> reused(before.size(), false);

  for (std::size_t index = 0; index < after.size(); ++index) {
    auto const found = by_key.find(key_of(after[index], index));

    if (found != by_key.end() && same_node(before[found->second], after[index])) {
      kept[index] = found->second;
      reused[found->second] = true;
    }
  }

  for (std::size_t index = 0; index < before.size(); ++index) {
    if (!reused[index]) {
      remove(before[index]);
    }
  }

  // From the end: each child goes before the next one, which is already in place. A kept child stays where it is if
  // it was already before all those that stayed.
  auto next = no_node;
  auto limit = std::numeric_limits<std::size_t>::max();

  for (auto index = after.size(); index-- > 0;) {
    auto &child = after[index];

    if (kept[index]) {
      patch(before[*kept[index]], child);

      if (*kept[index] < limit) {
        limit = *kept[index];
      } else {
        renderer_.insert(parent, child.id, next);
      }
    } else {
      create(child);
      renderer_.insert(parent, child.id, next);
    }

    next = child.id;
  }
}

void reconciler::create(element &value) {
  value.id = ++*last_id_;

  if (value.type == element::kind::text) {
    renderer_.create_text(value.id, value.content);
    return;
  }

  renderer_.create_element(value.id, value.tag);

  if (!value.classes.empty()) {
    renderer_.set_class(value.id, value.classes);
  }

  for (auto const &[name, content] : value.attributes) {
    renderer_.set_attribute(value.id, name, content);
  }

  if (value.value) {
    renderer_.set_value(value.id, *value.value);
  }

  for (auto const &binding : value.events) {
    if (first_of_its_name(value.events, binding)) {
      renderer_.listen(value.id, binding.name);
    }
  }

  for (auto &child : value.children) {
    create(child);
    renderer_.insert(value.id, child.id, no_node);
  }
}

void reconciler::patch(element &before, element &after) {
  after.id = before.id;

  if (after.type == element::kind::text) {
    if (before.content != after.content) {
      renderer_.set_text(after.id, after.content);
    }

    return;
  }

  if (before.classes != after.classes) {
    renderer_.set_class(after.id, after.classes);
  }

  patch_attributes(before, after);

  // An input no longer bound holds nothing rather than what it was last given.
  if (before.value != after.value) {
    renderer_.set_value(after.id, after.value.value_or(std::string{}));
  }

  patch_events(before, after);
  children(after.id, before.children, after.children);
}

void reconciler::patch_attributes(element const &before, element const &after) {
  for (auto const &[name, content] : before.attributes) {
    if (!after.attributes.contains(name)) {
      renderer_.remove_attribute(after.id, name);
    }
  }

  for (auto const &[name, content] : after.attributes) {
    auto const previous = before.attributes.find(name);

    if (previous == before.attributes.end() || previous->second != content) {
      renderer_.set_attribute(after.id, name, content);
    }
  }
}

void reconciler::patch_events(element const &before, element const &after) {
  for (auto const &binding : before.events) {
    if (first_of_its_name(before.events, binding) &&
        !std::ranges::contains(after.events, binding.name, &event_binding::name)) {
      renderer_.unlisten(after.id, binding.name);
    }
  }

  for (auto const &binding : after.events) {
    if (first_of_its_name(after.events, binding) &&
        !std::ranges::contains(before.events, binding.name, &event_binding::name)) {
      renderer_.listen(after.id, binding.name);
    }
  }
}

void reconciler::remove(element const &value) {
  for (auto const &child : value.children) {
    remove(child);
  }

  renderer_.remove(value.id);
}

} // namespace quoll::ui::detail
