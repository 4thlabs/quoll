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

#include <quoll/ui/detail/app_core.hpp>
#include <quoll/ui/detail/instance.hpp>
#include <quoll/ui/detail/view_context.hpp>

#include <utility>

namespace quoll::ui::detail {

namespace {

// Per thread: an app runs on one, and views run one inside another there.
thread_local view_context current{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

view_scope::view_scope(view_context context) noexcept : previous_(std::exchange(current, std::move(context))) {}

view_scope::~view_scope() { current = std::move(previous_); }

call_target current_target() noexcept {
  return {.app = current.app,
          .owner = current.held.owner,
          .props = current.held.props,
          .of_component = current.held.owner != nullptr};
}

bool spawn_in(call_target const &target, async::task<> work) {
  auto *const core = target.app ? target.app->core : nullptr;
  auto owner = target.owner.lock();

  // A component gone, or going, as its stop says: its work does not start.
  if (core == nullptr || core->stopped() || (target.of_component && (!owner || owner->stop_token().stop_requested()))) {
    return false;
  }

  core->spawn(std::move(work), {.owner = std::move(owner), .props = target.props});
  return true;
}

async::task<> done() { co_return; }

} // namespace quoll::ui::detail
