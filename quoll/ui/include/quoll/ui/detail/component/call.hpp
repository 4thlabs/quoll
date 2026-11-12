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

#ifndef QUOLL_UI_DETAIL_COMPONENT_CALL_HPP
#define QUOLL_UI_DETAIL_COMPONENT_CALL_HPP

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/component/call_state.hpp>
#include <quoll/ui/detail/view/view_context.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

// The work of a call, as a task of what `target` named: what `function` gives or throws reaches the caller through
// `state`, unless it stopped waiting, its failure then thrown again for the app to report.
template <class R, class Function, class... Args>
[[nodiscard]] async::task<> call_work(std::shared_ptr<Function const> function, std::shared_ptr<call_state<R>> state,
                                      Args... args) {
  call_end<R> end{std::move(state)};

  // What it runs as stopped since the call was posted: it does not run, and the caller ends on the stop with it.
  auto const owner_stop = co_await async::current_stop_token();

  if (owner_stop.stop_requested()) {
    co_return;
  }

  try {
    if constexpr (std::is_void_v<R>) {
      co_await (*function)(std::move(args)...);
      end.give({});
    } else {
      end.give(co_await (*function)(std::move(args)...));
    }
  } catch (...) {
    // A stop a function throws while what it runs as stops is a stop: the caller ends on it, as no catch sees one.
    if (owner_stop.stop_requested() && async::detail::is_a_stop(std::current_exception())) {
      co_return;
    }

    if (!end.fail(std::current_exception())) {
      throw;
    }
  }
}

// Calls `function` with `args` as a task of what `target` names, a component or an app, and waits for its end: a
// caller that stops stops waiting, and the work goes on, as it belongs to what it runs as. Owns what it needs: what
// made the call may go before the task ends. With no app, as a view written once, it runs in the caller's task.
template <class R, class Function, class... Args>
[[nodiscard]] async::task<R> call_in(std::shared_ptr<Function const> function, call_target target, Args... args) {
  if (!function) {
    throw std::bad_function_call{};
  }

  // Stopped already: it asks for nothing.
  auto const caller_stop = co_await async::current_stop_token();

  if (caller_stop.stop_requested()) {
    co_await async::detail::end_stopped{};
  }

  if (!target.app) {
    // Made in a component's view for to_html: the component is gone with the page.
    if (target.of_component) {
      co_await async::detail::end_stopped{};
    }

    if constexpr (std::is_void_v<R>) {
      co_await (*function)(std::move(args)...);
      co_return;
    } else {
      co_return co_await (*function)(std::move(args)...);
    }
  }

  auto state = std::make_shared<call_state<R>>();

  if (!spawn_in(target, call_work<R>(std::move(function), state, std::move(args)...))) {
    co_await async::detail::end_stopped{};
  }

  if constexpr (std::is_void_v<R>) {
    co_await call_wait<R>{std::move(state)};
  } else {
    co_return co_await call_wait<R>{std::move(state)};
  }
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_COMPONENT_CALL_HPP
