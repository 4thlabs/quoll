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

#ifndef QUOLL_ASYNC_DETAIL_WHEN_ALL_AWAITER_HPP
#define QUOLL_ASYNC_DETAIL_WHEN_ALL_AWAITER_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/detail/when_all_runner.hpp>
#include <quoll/async/detail/when_all_state.hpp>
#include <quoll/async/task.hpp>

#include <array>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <tuple>
#include <utility>

namespace quoll::async::detail {

// Holds the children, their results and the join state; awaited as an lvalue, as it cannot move.
template <class... Ts>
class when_all_awaiter {
public:
  explicit when_all_awaiter(task<Ts>... children) : children_(std::move(children)...) {}

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent, await_context const &context) {
    state_.set_parent(parent);

    // Every frame is allocated before any child runs: if an allocation throws, nothing has started.
    auto runners = [this]<std::size_t... I>(std::index_sequence<I...>) {
      return std::array<when_all_runner, sizeof...(Ts)>{
          run_when_all_child(std::move(std::get<I>(children_)), std::get<I>(slots_), state_)...};
    }(std::index_sequence_for<Ts...>{});

    for (auto &runner : runners) {
      runner.start(context, state_);
    }
    return state_.arrive();
  }

  [[nodiscard]] std::tuple<non_void_t<Ts>...> await_resume() {
    state_.rethrow_if_failed();
    return [this]<std::size_t... I>(std::index_sequence<I...>) {
      return std::tuple<non_void_t<Ts>...>{std::move(*std::get<I>(slots_))...};
    }(std::index_sequence_for<Ts...>{});
  }

private:
  std::tuple<task<Ts>...> children_;
  std::tuple<std::optional<non_void_t<Ts>>...> slots_;
  when_all_state state_{sizeof...(Ts)};
};

static_assert(ContextAwaitable<when_all_awaiter<int, void>>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_WHEN_ALL_AWAITER_HPP
