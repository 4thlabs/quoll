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

#include <quoll/emscripten/browser_history.hpp>
#include <quoll/emscripten/detail/js.hpp>

#include <quoll/async/detail/type_id.hpp>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace quoll::emscripten {

namespace {

// A listener of the page's history, told through a function JavaScript can call.
struct listening {
  void *listener;
  void (*changed)(void *listener, std::string_view href);
};

// The listeners of the page's history, by the listener: one page, on its one thread.
std::map<void *, std::unique_ptr<listening>> &listeners() {
  static std::map<void *, std::unique_ptr<listening>> all;

  return all;
}

void told(void *entry, char const *href) {
  auto const &held = *static_cast<listening *>(entry);

  held.changed(held.listener, href);
}

// What browser_history() refers to, for history_ref::target().
struct browser {};

constexpr ui::history_ref::table browser_table{
    .type = async::detail::type_id_of<browser>(),
    .current =
        [](void *) {
          auto *const href = detail::quoll_emscripten_history_location();
          std::string current{href};

          std::free(href);
          return current;
        },
    .push = [](void *, std::string_view href) { detail::quoll_emscripten_history_push(href.data(), href.size()); },
    .replace = [](void *,
                  std::string_view href) { detail::quoll_emscripten_history_replace(href.data(), href.size()); },
    .back = [](void *) { detail::quoll_emscripten_history_back(); },
    .listen =
        [](void *, void *listener, void (*changed)(void *, std::string_view)) {
          auto &entry = listeners()[listener];

          // Listening again takes the place of the listening before, whose handler JavaScript lets go of first.
          if (entry) {
            detail::quoll_emscripten_history_unlisten(entry.get());
          }

          entry = std::make_unique<listening>(listening{.listener = listener, .changed = changed});
          detail::quoll_emscripten_history_listen(&told, entry.get());
        },
    .unlisten =
        [](void *, void *listener) {
          auto &all = listeners();

          if (auto const found = all.find(listener); found != all.end()) {
            detail::quoll_emscripten_history_unlisten(found->second.get());
            all.erase(found);
          }
        },
};

} // namespace

ui::history_ref browser_history() noexcept { return {nullptr, browser_table}; }

} // namespace quoll::emscripten
