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

#include <quoll/ui/context_spec.hpp>
#include <quoll/ui/detail/context_entry.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace quoll::ui::detail {

context_entry::context_entry(context_table const &table, state_link app) : context_entry(table, app, table.make()) {}

context_entry::context_entry(context_table const &table, state_link app, held_component object)
    : table_(&table), app_(app), object_(std::move(object)) {
  table_->link(object_.get(), {this, &context_entry::changed_in});
}

context_entry::~context_entry() { table_->link(object_.get(), {}); }

void context_entry::changed_in(void *self) {
  auto &changed = *static_cast<context_entry *>(self);

  ++changed.version_;
  tell(changed.app_);
}

std::uint64_t context_entry::version() const noexcept {
  return version_ + (table_->contexts_version == nullptr ? 0 : table_->contexts_version(object_.get()));
}

context_list::~context_list() {
  while (!entries_.empty()) {
    entries_.pop_back();
  }
}

void context_list::push_back(std::unique_ptr<context_entry> entry) { entries_.push_back(std::move(entry)); }

} // namespace quoll::ui::detail

namespace quoll::ui {

detail::held_component context_spec::make() const { return make_ ? make_() : table_->make(); }

} // namespace quoll::ui
