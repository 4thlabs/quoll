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

#ifndef QUOLL_UI_DETAIL_INSTANCE_HPP
#define QUOLL_UI_DETAIL_INSTANCE_HPP

#include <quoll/async/detail/stop_waker.hpp>
#include <quoll/ui/detail/component_table.hpp>

#include <exception>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace quoll::ui::detail {

// Where what a component's unmounted() throws goes: the app's report of what a handler throws.
struct failure_sink {
  void *owner = nullptr;
  void (*failed)(void *owner, std::exception_ptr failure) noexcept = nullptr;
};

// A component of a view, kept from one view to the next while its parent's view has it at the same place: its own
// states, its props as the parent last gave them, the components of its own view, and a stop of its own, which the
// app's stops too. Shared: the app holds it while it shows, and each of its tasks until it ends, so that a component
// that goes outlives what runs on it. Its props hold the component that placed it, which they may refer to, so that a
// parent outlives what runs on its children with props: wherever the props of its children are, a task or a binding,
// they hold it, and its children hold it so, which is the cycle detach() breaks. A child without props refers to
// nothing of it.
//
// Thread safety: none, as its app.
class instance {
public:
  instance(component_table const &table, std::shared_ptr<void const> props, std::string key,
           std::stop_token const &app_stop);

  instance(instance const &) = delete;
  instance(instance &&) = delete;
  instance &operator=(instance const &) = delete;
  instance &operator=(instance &&) = delete;

  ~instance() = default;

  [[nodiscard]] component_table const &table() const noexcept { return *table_; }

  [[nodiscard]] void *component() const noexcept { return component_.get(); }

  // As the parent last gave them: a task holding them keeps them as they were when it started.
  [[nodiscard]] std::shared_ptr<void const> const &props() const noexcept { return props_; }

  // Given to a view that returned, shown or not: those before go, as the component may refer to `given` only until its
  // next view that returns.
  void set_props(std::shared_ptr<void const> props) noexcept;

  [[nodiscard]] std::string const &key() const noexcept { return key_; }

  [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_.get_token(); }

  [[nodiscard]] std::vector<std::shared_ptr<instance>> &children() noexcept { return children_; }

  // It goes: its tasks are asked to stop, then the components of its view go, then its unmounted() runs; what one
  // throws goes to `sink`, and the rest go all the same. Its tasks end on their own; what holds it then is them.
  void unmount(failure_sink sink) noexcept;

  // Lets go of the components of its view, and they of theirs, without running anything of them: for an app that is
  // gone, whose components have been unmounted, so that a child, whose props hold its parent, leaves no cycle.
  void detach() noexcept;

private:
  void unlink() noexcept;

  component_table const *table_;
  // Before the component, which may keep what refers to them.
  std::shared_ptr<void const> props_;
  held_component component_;
  std::string key_;
  // Before what stops it.
  std::stop_source stop_;
  std::stop_callback<async::detail::stop_waker> follow_app_;
  std::vector<std::shared_ptr<instance>> children_;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_INSTANCE_HPP
