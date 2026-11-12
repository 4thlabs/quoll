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

#include <quoll/ui/detail/view/reconciler.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace quoll::ui::detail {

namespace {

// Whether two lists of elements show the same, their descendants included: what a select's options hold and say.
bool same_content(std::vector<element> const &before, std::vector<element> const &after) {
  return std::ranges::equal(before, after, [](element const &one, element const &other) {
    return one.type == other.type && one.tag == other.tag && one.content == other.content &&
           one.attributes == other.attributes && one.value == other.value && one.checked == other.checked &&
           one.key == other.key && same_content(one.children, other.children);
  });
}

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
  return before.type == after.type && before.tag == after.tag &&
         (before.type != element::kind::widget || before.widget->module == after.widget->module);
}

// An element may run several functions on one event: a renderer listens to it once, or each would run several times.
[[nodiscard]] bool first_of_its_name(std::vector<event_binding> const &events, event_binding const &binding) {
  return &*std::ranges::find(events, binding.name, &event_binding::name) == &binding;
}

// What a render may not show, checked before any operation of the renderer, so that a refused render changes nothing.
void check_widgets(std::vector<element> const &elements, bool shows_widgets) {
  for (auto const &value : elements) {
    if (value.type == element::kind::widget) {
      if (value.widget == nullptr) {
        throw std::logic_error{"quoll::ui: a widget with no module: widget<Props> makes one"};
      }

      if (!value.children.empty()) {
        throw std::logic_error{"quoll::ui: a widget's node is its module's: it takes no children"};
      }

      if (!shows_widgets) {
        throw std::logic_error{"quoll::ui: a widget placed through a renderer that shows none"};
      }
    }

    check_widgets(value.children, shows_widgets);
  }
}

} // namespace

void reconciler::apply(node_id parent, std::vector<element> &before, std::vector<element> &after) {
  check_widgets(after, renderer_.shows_widgets());
  children(parent, before, after);
}

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

  for (auto const &binding : value.events) {
    if (first_of_its_name(value.events, binding)) {
      renderer_.listen(value.id, binding.name);
    }
  }

  // Its children are the module's.
  if (value.type == element::kind::widget) {
    set_properties(value);
    widgets_.mounts.emplace_back(value.id, value.widget);
    return;
  }

  for (auto &child : value.children) {
    create(child);
    renderer_.insert(value.id, child.id, no_node);
  }

  // Once its children are there: a select chooses among its options.
  set_properties(value);
}

void reconciler::set_properties(element const &value) {
  if (value.value) {
    renderer_.set_value(value.id, *value.value);
  }

  if (value.checked) {
    renderer_.set_checked(value.id, *value.checked);
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
  patch_events(before, after);

  if (after.type == element::kind::widget) {
    patch_properties(before, after);

    if (before.widget->props != after.widget->props) {
      widgets_.updates.emplace_back(after.id, after.widget);
    }

    return;
  }

  children(after.id, before.children, after.children);
  patch_properties(before, after);
}

void reconciler::patch_properties(element const &before, element const &after) {
  // A select whose options changed, in what they hold or say, within a group too, is given its value again, as the
  // one it named may be there now; otherwise what the user chose stays while the value given does.
  // An input no longer bound holds nothing rather than what it was last given.
  if (before.value != after.value ||
      (after.value && after.tag == "select" && !same_content(before.children, after.children))) {
    renderer_.set_value(after.id, after.value.value_or(std::string{}));
  }

  // One no longer told whether it is checked is not.
  if (before.checked != after.checked) {
    renderer_.set_checked(after.id, after.checked.value_or(false));
  }
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
  // Taken back from its module before the node goes; what that throws is told once the render shows, and leaves the
  // rest of the render.
  if (value.type == element::kind::widget) {
    try {
      renderer_.unmount_widget(value.id);
    } catch (...) {
      widgets_.failures.push_back(std::current_exception());
    }
  }

  for (auto const &child : value.children) {
    remove(child);
  }

  renderer_.remove(value.id);
}

} // namespace quoll::ui::detail
