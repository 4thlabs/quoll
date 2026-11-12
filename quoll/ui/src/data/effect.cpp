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
  watch_(watching::forget, nullptr);
  detail::tell(link_);
}

void effect::after_view(detail::effect_starter const &start) {
  // Failed to start: it waits for reload(), rather than be tried after each view, which a lack lasting would make
  // without end, as its failure asks for a render.
  if (failed_) {
    return;
  }

  auto const *const props = start.props;
  bool const wrong_props =
      props_type_ != async::detail::type_id{} && (props_type_ != start.props_type || props == nullptr);

  // Before the first view: loading if its first run will start, which wrong props, refused at that view, do not.
  if (start.first) {
    if (!wrong_props && watch_(watching::peek, props)->holds) {
      target_.begin();
    }

    return;
  }

  // On the props of another component, or a field of a base of them, or of an app's own component, which has none: a
  // mistake of the program, refused at its first view, to the app's on_error whatever its target holds, which was not
  // loading; then waiting for reload(), as its type tells no more.
  if (wrong_props) {
    failed_ = true;
    throw std::logic_error{"quoll::ui::effect: it depends on a field of props its component does not have"};
  }

  // One way out for what fails, whatever it is, comparing or copying its states, or starting: a view in which they
  // changed, or may have, stops the run before, which would give what the values before ask for.
  try {
    auto const now = *watch_(watching::record, props);

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
    start(run_(detail::ask(slot_), props));
  } catch (...) {
    // Its failure, which its target shows, and the app's on_error hears of.
    stop_before();
    failed_ = true;
    detail::tell_failure(target_, detail::failure_to::app, std::current_exception());
  }
}

void effect::stop_before() noexcept {
  slot_.last.request_stop();
  slot_.pending = false;
}

} // namespace quoll::ui
