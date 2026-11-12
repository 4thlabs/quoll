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

#include <quoll/ui/detail/run_slot.hpp>

#include <exception>
#include <stop_token>
#include <utility>

namespace quoll::ui::detail {

void tell_failure(target_hooks target, untold failures, std::exception_ptr const &failure) {
  if (!target.fail(failure) && failures == untold::thrown) {
    std::rethrow_exception(failure);
  }
}

std::stop_token ask(run_slot &slot) {
  // Made first: one that cannot be made, as for a lack of memory, leaves the run before as it was.
  std::stop_source next;

  slot.last.request_stop();
  slot.last = std::move(next);
  return slot.last.get_token();
}

run_guard::run_guard(run_slot &slot, std::stop_token mine, target_hooks target) noexcept
    : slot_(slot), mine_(std::move(mine)), target_(target) {
  ++slot_.frames;
}

run_guard::~run_guard() {
  --slot_.frames;

  if (!mine_.stop_requested() && slot_.pending) {
    slot_.pending = false;
    target_.abandon();
  }
}

} // namespace quoll::ui::detail
