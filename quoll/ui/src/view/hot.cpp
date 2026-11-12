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

#include <quoll/ui/detail/view/hot.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quoll::ui::detail {

namespace {

// What was replaced so far, and whom to tell: for the whole program, as what it replaces is the program's source. A
// literal written at its call has no context to reach anything else through.
struct hot_state {
  std::unordered_map<std::string, std::string> literals;
  std::vector<std::pair<void *, void (*)(void *)>> listeners;
};

[[nodiscard]] hot_state &hot() {
  static hot_state state;

  return state;
}

[[nodiscard]] std::string hot_key(std::string_view file, unsigned line, unsigned column) {
  return std::string{file} + ':' + std::to_string(line) + ':' + std::to_string(column);
}

} // namespace

void hot_replace(std::string_view file, unsigned line, unsigned column, std::string_view value) {
  hot().literals.insert_or_assign(hot_key(file, line, column), std::string{value});

  for (auto const &[owner, changed] : hot().listeners) {
    changed(owner);
  }
}

std::optional<std::string_view> hot_find(std::string_view file, unsigned line, unsigned column) {
  auto const found = hot().literals.find(hot_key(file, line, column));

  if (found == hot().literals.end()) {
    return std::nullopt;
  }

  return std::string_view{found->second};
}

void hot_subscribe(void *owner, void (*changed)(void *)) { hot().listeners.emplace_back(owner, changed); }

void hot_unsubscribe(void *owner) {
  std::erase_if(hot().listeners, [owner](auto const &listener) { return listener.first == owner; });
}

} // namespace quoll::ui::detail
