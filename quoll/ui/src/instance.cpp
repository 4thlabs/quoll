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

void instance::tried(std::shared_ptr<void const> props) noexcept {
  last_view_.reset();
  props_ = std::move(props);
  own_.reset();
  placed_by_.reset();
}

void instance::viewed(std::shared_ptr<void const> props, std::shared_ptr<void const> own,
                      std::shared_ptr<void const> placed_by, element view, std::uint64_t version) noexcept {
  // The view first, whose handlers may refer to the props it replaces.
  last_view_.reset();
  props_ = std::move(props);
  own_ = std::move(own);
  placed_by_ = std::move(placed_by);
  last_view_.emplace(std::move(view));
  viewed_ = version;
}

bool instance::made_of(void const *own, void const *placed_by) const noexcept {
  return last_view_ && own == own_.get() && placed_by == placed_by_.get();
}

element const *instance::unchanged_view() const noexcept {
  return last_view_ && version_ == viewed_ ? &*last_view_ : nullptr;
}

void instance::link(state_link app) noexcept {
  app_link_ = app;
  table_->link(component_.get(), {this, &instance::changed_in});
}

void instance::changed_in(void *self) {
  auto &changed = *static_cast<instance *>(self);

  ++changed.version_;

  if (changed.app_link_.changed != nullptr) {
    changed.app_link_.changed(changed.app_link_.owner);
  }
}

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
  app_link_ = {};
}

void instance::detach() noexcept {
  for (auto const &child : children_) {
    child->detach();
  }

  children_.clear();
}

} // namespace quoll::ui::detail
