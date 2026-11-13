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

#include <quoll/ui/detail/component/instance.hpp>
#include <quoll/ui/detail/context/bind_contexts.hpp>
#include <quoll/ui/detail/data/run_effects.hpp>

#include <utility>

namespace quoll::ui::detail {

instance::instance(
    component_table const &table, std::shared_ptr<void const> props, std::string key, std::stop_token const &app_stop,
    context_finder const &contexts
)
    : table_(&table), props_(std::move(props)), component_(table.make()), key_(std::move(key)),
      follow_app_(app_stop, async::detail::stop_waker{&stop_}),
      parent_(std::static_pointer_cast<instance const>(contexts.from)) {
  if (table_->bind != nullptr) {
    table_->bind(component_.get(), contexts);
  }

  // Counted from the start, its first view and what made it included, so that a change there views it again; its
  // app hears of changes once it shows.
  table_->link(component_.get(), {this, &instance::changed_in});
  // Made, its first view next: the targets of the effects whose first run will start say they are loading.
  first_effects(table_->effects, component_.get());
}

void instance::tried(std::shared_ptr<void const> props) noexcept {
  last_view_.reset();
  props_ = std::move(props);
  own_.reset();
  placed_by_.reset();
}

void instance::viewed(
    std::shared_ptr<void const> props, std::shared_ptr<void const> own, std::shared_ptr<void const> placed_by,
    element view, std::uint64_t version, std::uint64_t epoch
) noexcept {
  // The view first, whose handlers may refer to the props it replaces.
  last_view_.reset();
  props_ = std::move(props);
  own_ = std::move(own);
  placed_by_ = std::move(placed_by);
  last_view_.emplace(std::move(view));
  viewed_ = version;
  viewed_epoch_ = epoch;

  if (table_->commit_contexts != nullptr) {
    table_->commit_contexts(component_.get());
  }
}

bool instance::made_of(void const *own, void const *placed_by) const noexcept {
  return last_view_ && own == own_.get() && placed_by == placed_by_.get();
}

void instance::mark_contexts() noexcept {
  if (table_->mark_contexts != nullptr) {
    table_->mark_contexts(component_.get());
  }
}

element const *instance::unchanged_view(std::uint64_t epoch) const noexcept {
  // Its contexts last, which costs a walk of its members.
  if (!last_view_ || version_ != viewed_ || epoch != viewed_epoch_) {
    return nullptr;
  }

  return table_->contexts_changed != nullptr && table_->contexts_changed(component_.get()) ? nullptr : &*last_view_;
}

void instance::link(state_link app) noexcept { app_link_ = app; }

found_context instance::provided_above(std::shared_ptr<void const> const &from, async::detail::type_id type) noexcept {
  for (auto at = std::static_pointer_cast<instance const>(from); at; at = at->parent_.lock()) {
    if (at->table_->provides == nullptr) {
      continue;
    }

    if (auto *const entry = at->table_->provides(at->component_.get(), type)) {
      return {.entry = entry, .keeper = at};
    }
  }

  return {};
}

void instance::changed_in(void *self) {
  auto &changed = *static_cast<instance *>(self);

  ++changed.version_;

  if (changed.app_link_.changed != nullptr) {
    changed.app_link_.changed(changed.app_link_.owner);
  }
}

void instance::unmount() noexcept {
  // The components of its view first, then its own tasks, as React cleans up children first: their runs unwind in
  // that order, the stops posted as they are asked for.
  for (auto const &child : children_) {
    child->unmount();
  }

  children_.clear();
  stop_.request_stop();
  unlink();
}

instance::~instance() { table_->link(component_.get(), {}); }

void instance::unlink() noexcept {
  // Gone from the view: what its tasks, or its destructor, still change shows nowhere.
  table_->link(component_.get(), {});
  app_link_ = {};
  last_view_.reset();
  own_.reset();
  placed_by_.reset();
}

void instance::detach() noexcept {
  for (auto const &child : children_) {
    child->detach();
  }

  children_.clear();
}

} // namespace quoll::ui::detail
