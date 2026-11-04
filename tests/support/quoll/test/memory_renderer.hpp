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

#ifndef QUOLL_TEST_MEMORY_RENDERER_HPP
#define QUOLL_TEST_MEMORY_RENDERER_HPP

#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::test {

/**
 * @brief A Renderer keeping the interface as a tree in memory, for the tests of what shows: html() writes it, and
 * operations() counts what was asked of it. Copies share the tree: a test keeps one to look at what the app shows.
 */
class memory_renderer {
public:
  void create_element(ui::node_id id, std::string_view tag) {
    ++state_->operations;
    state_->nodes[id].tag = tag;
  }

  void create_text(ui::node_id id, std::string_view content) {
    ++state_->operations;
    state_->nodes[id].content = content;
  }

  void set_text(ui::node_id id, std::string_view content) {
    ++state_->operations;
    state_->nodes.at(id).content = content;
  }

  void set_class(ui::node_id id, std::string_view classes) {
    ++state_->operations;
    state_->nodes.at(id).classes = classes;
  }

  void set_attribute(ui::node_id id, std::string_view name, std::string_view value) {
    ++state_->operations;
    state_->nodes.at(id).attributes[std::string{name}] = value;
  }

  void remove_attribute(ui::node_id id, std::string_view name) {
    ++state_->operations;
    state_->nodes.at(id).attributes.erase(std::string{name});
  }

  void set_value(ui::node_id id, std::string_view value) {
    ++state_->operations;
    state_->nodes.at(id).attributes["value"] = value;
  }

  void listen(ui::node_id id, std::string_view name) {
    ++state_->operations;
    state_->nodes.at(id).events.emplace_back(name);
  }

  void unlisten(ui::node_id id, std::string_view name) {
    ++state_->operations;
    std::erase(state_->nodes.at(id).events, name);
  }

  void insert(ui::node_id parent, ui::node_id child, ui::node_id before) {
    ++state_->operations;
    detach(child);

    auto &siblings = state_->nodes[parent].children;
    auto const place = before == ui::no_node ? siblings.end() : std::ranges::find(siblings, before);

    siblings.insert(place, child);
    state_->nodes.at(child).parent = parent;
  }

  void remove(ui::node_id id) {
    ++state_->operations;
    detach(id);
    state_->nodes.erase(id);
  }

  /** @brief What shows, as HTML. */
  [[nodiscard]] std::string html() const {
    std::string written;

    for (auto const child : state_->nodes[ui::no_node].children) {
      write(child, written);
    }

    return written;
  }

  [[nodiscard]] int operations() const noexcept { return state_->operations; }

  void reset_operations() noexcept { state_->operations = 0; }

  /** @brief The id of an element with this tag: what an event is sent to. */
  [[nodiscard]] ui::node_id find(std::string_view tag) const {
    for (auto const &[id, value] : state_->nodes) {
      if (value.tag == tag) {
        return id;
      }
    }

    return ui::no_node;
  }

  /** @brief The id of the element holding the text `content`. */
  [[nodiscard]] ui::node_id find_text(std::string_view content) const {
    for (auto const &[id, value] : state_->nodes) {
      if (value.tag.empty() && value.content == content) {
        return value.parent;
      }
    }

    return ui::no_node;
  }

  [[nodiscard]] std::vector<std::string> events(ui::node_id id) const { return state_->nodes.at(id).events; }

private:
  struct node {
    std::string tag{};
    std::string content{};
    std::string classes{};
    std::map<std::string, std::string> attributes{};
    std::vector<std::string> events{};
    std::vector<ui::node_id> children{};
    ui::node_id parent = ui::no_node;
  };

  struct shared_state {
    std::map<ui::node_id, node> nodes;
    int operations = 0;
  };

  void detach(ui::node_id id) {
    auto const found = state_->nodes.find(id);

    if (found != state_->nodes.end()) {
      std::erase(state_->nodes[found->second.parent].children, id);
    }
  }

  void write(ui::node_id id, std::string &written) const {
    auto const &value = state_->nodes.at(id);

    if (value.tag.empty()) {
      written += value.content;
      return;
    }

    written += '<';
    written += value.tag;

    if (!value.classes.empty()) {
      written += " class=\"" + value.classes + '"';
    }

    for (auto const &[name, content] : value.attributes) {
      written += ' ' + name + "=\"" + content + '"';
    }

    written += '>';

    for (auto const child : value.children) {
      write(child, written);
    }

    written += "</" + value.tag + '>';
  }

  std::shared_ptr<shared_state> state_ = std::make_shared<shared_state>();
};

static_assert(ui::Renderer<memory_renderer>);

} // namespace quoll::test

#endif // QUOLL_TEST_MEMORY_RENDERER_HPP
