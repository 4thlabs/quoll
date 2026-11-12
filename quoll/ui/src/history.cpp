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

#include <quoll/ui/history.hpp>

#include <quoll/async/detail/type_id.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace quoll::ui {

memory_history::memory_history(std::string start) : entries_{std::move(start)} {}

history_ref memory_history::ref() noexcept {
  // In a member function, so that its entries reach the members of what they are given.
  static constexpr history_ref::table table{
      .type = async::detail::type_id_of<memory_history>(),
      .current = [](void *self) { return static_cast<memory_history *>(self)->current(); },
      .push = [](void *self, std::string_view href) { static_cast<memory_history *>(self)->push(href); },
      .replace = [](void *self, std::string_view href) { static_cast<memory_history *>(self)->replace(href); },
      .back = [](void *self) { static_cast<memory_history *>(self)->back(); },
      .listen =
          [](void *self, void *listener, void (*changed)(void *, std::string_view)) {
            auto &history = *static_cast<memory_history *>(self);

            history.listener_ = listener;
            history.changed_ = changed;
          },
      .unlisten =
          [](void *self, void *listener) {
            auto &history = *static_cast<memory_history *>(self);

            if (history.listener_ == listener) {
              history.listener_ = nullptr;
              history.changed_ = nullptr;
            }
          },
  };

  return {this, table};
}

void memory_history::push(std::string_view href) {
  entries_.resize(at_ + 1);
  entries_.emplace_back(href);
  ++at_;
}

void memory_history::replace(std::string_view href) { entries_[at_] = std::string{href}; }

void memory_history::back() {
  if (at_ == 0) {
    return;
  }

  --at_;

  if (changed_ != nullptr) {
    changed_(listener_, entries_[at_]);
  }
}

} // namespace quoll::ui
