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

#include <quoll/ui/data/effect.hpp>

#include <cassert>
#include <exception>
#include <stdexcept>

namespace quoll::ui {

effect::~effect() { assert(slot_.frames == 0 && "quoll::ui::effect: destroyed while a run is under way"); }

void effect::reload() {
  failed_ = false;
  started_ = false;
  watch_(watching::forget);
  detail::tell(link_);
}

void effect::after_view(detail::effect_starter const &start) {
  // Failed to start: it waits for reload(), rather than be tried after each view, which a lack lasting would make
  // without end, as its failure asks for a render.
  if (failed_) {
    return;
  }

  // Before the first view: loading if its first run will start.
  if (start.first) {
    if (watch_(watching::peek)->holds) {
      target_.begin();
    }

    return;
  }

  // One way out for what fails, whatever it is, comparing or copying its states, or starting: a view in which they
  // changed, or may have, stops the run before, which would give what the values before ask for.
  try {
    auto const now = *watch_(watching::record);

    if (!now.differs) {
      return;
    }

    // None held: the run before stops, and the target is loading no more.
    if (!now.holds) {
      stop_before();
      target_.abandon();
      return;
    }

    // Asked for now, as the view is shown: the run before stops at once, so that an answer of its that comes before
    // this one starts writes nothing.
    start(run_(detail::ask(slot_), started_));
    started_ = true;
  } catch (...) {
    fail_start(std::current_exception());
  }
}

void effect::fail_start(std::exception_ptr const &failure) {
  stop_before();
  failed_ = true;
  detail::tell_failure(target_, detail::failure_to::app, failure);
  // tell_failure throws for failure_to::app: what follows does not run.
  std::rethrow_exception(failure);
}

void detail::effect_access::fail(effect &run, std::exception_ptr const &failure) { run.fail_start(failure); }

bool detail::effect_access::failed(effect const &run) noexcept { return run.failed_; }

void effect::stop_before() noexcept {
  slot_.last.request_stop();
  slot_.pending = false;
}

} // namespace quoll::ui
