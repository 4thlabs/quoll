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

#include <quoll/ui/detail/instance.hpp>

#include <utility>

namespace quoll::ui::detail {

instance::instance(component_table const &table, std::shared_ptr<void const> props, std::string key,
                   std::stop_token const &app_stop)
    : table_(&table), props_(std::move(props)), component_(table.make()), key_(std::move(key)),
      follow_app_(app_stop, async::detail::stop_waker{&stop_}) {}

void instance::set_props(std::shared_ptr<void const> props) noexcept { props_ = std::move(props); }

void instance::unmount(failure_sink sink) noexcept {
  // First, so that nothing of it runs on past a failure below.
  stop_.request_stop();

  for (auto const &child : children_) {
    child->unmount(sink);
  }

  children_.clear();

  try {
    if (table_->unmounted != nullptr) {
      table_->unmounted(component_.get());
    }
  } catch (...) {
    if (sink.failed != nullptr) {
      sink.failed(sink.owner, std::current_exception());
    }
  }

  unlink();
}

void instance::unlink() noexcept {
  // Gone from the view: what its tasks, or its destructor, still change shows nowhere.
  table_->link(component_.get(), {});
}

void instance::detach() noexcept {
  for (auto const &child : children_) {
    child->detach();
  }

  children_.clear();
}

} // namespace quoll::ui::detail
