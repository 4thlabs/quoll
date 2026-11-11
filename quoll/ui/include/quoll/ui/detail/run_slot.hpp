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

#ifndef QUOLL_UI_DETAIL_RUN_SLOT_HPP
#define QUOLL_UI_DETAIL_RUN_SLOT_HPP

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/either_stop.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/state_link.hpp>
#include <quoll/ui/run_error.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

// The runs of an effect, an action, or an action's key: whether one is under way, whether the last one was stopped by
// its caller, what the last one that ended gave or threw, the stop of the one under way, and its number, which a later
// one takes.
template <class R>
struct run_slot {
  bool pending = false;
  bool stopped = false;
  std::optional<std::expected<R, std::exception_ptr>> result;
  // None until a run asks for one, so that a slot made costs no allocation.
  std::stop_source last{std::nostopstate};
  std::uint64_t current = 0;
  // The frames of its runs still alive, ended or not: what its holder must outlive.
  std::uint32_t frames = 0;
};

// Tells the app of a run's change: a render that cannot be asked for now is asked by the next change, so that what the
// run gives still reaches its caller, and a frame going still says it ended.
void tell_quietly(state_link const &link) noexcept;

// A function to run, refused empty rather than at each run.
template <class Function>
void check_function(Function const &function, char const *message) {
  if (!function) {
    throw std::invalid_argument{message};
  }
}

// A run that goes without ending, its frame destroyed by a stop of its caller, is under way no more, and stopped,
// unless a later run took its place. Neither copied nor moved: it lives in the run's frame.
template <class R>
class run_guard {
public:
  run_guard(run_slot<R> &slot, state_link const &link, std::uint64_t number) noexcept
      : slot_(slot), link_(link), number_(number) {
    ++slot_.frames;
  }

  run_guard(run_guard const &) = delete;
  run_guard(run_guard &&) = delete;
  run_guard &operator=(run_guard const &) = delete;
  run_guard &operator=(run_guard &&) = delete;

  ~run_guard() {
    --slot_.frames;

    if (slot_.current == number_ && slot_.pending) {
      slot_.pending = false;
      slot_.stopped = true;
      tell_quietly(link_);
    }
  }

private:
  run_slot<R> &slot_;
  state_link const &link_;
  std::uint64_t number_;
};

// Runs what `make` gives for `slot`, after stopping the run before, or refuses at once while one is under way if
// `refuse`: what it gave or threw, written to the slot, what `make` throws included, unless a later run took its place,
// which gives run_replaced and writes nothing, as for a run stopped by it. A stop of the caller, or of its component,
// ends the caller there, as a stop does, and leaves the slot stopped. `forget` empties the result as it starts. The
// slot and the link outlive the run: members of a component, which outlives its tasks.
template <class R, class Make>
  requires std::same_as<std::invoke_result_t<Make &>, async::task<R>>
async::task<std::expected<R, std::exception_ptr>> run_in(run_slot<R> &slot, state_link const &link, Make make,
                                                         bool refuse, bool forget) {
  if (refuse && slot.pending) {
    co_return std::unexpected{std::make_exception_ptr(run_refused{})};
  }

  if (slot.pending) {
    slot.last.request_stop();
  }

  slot.last = std::stop_source{};

  auto const number = ++slot.current;

  slot.pending = true;
  slot.stopped = false;

  if (forget) {
    slot.result.reset();
  }

  run_guard<R> const guard{slot, link, number};

  tell_quietly(link);

  auto const mine = slot.last.get_token();
  auto const caller = co_await async::current_stop_token();
  async::detail::either_stop const stops{caller, mine};
  std::optional<std::expected<R, std::exception_ptr>> ended;

  try {
    auto value = co_await async::detail::owned_chain<R>{make(), stops.token()};

    if (value) {
      if constexpr (std::is_void_v<R>) {
        ended.emplace();
      } else {
        ended.emplace(std::move(*value));
      }
    }
  } catch (...) {
    ended.emplace(std::unexpect, std::current_exception());
  }

  // Stopped by its caller: it ends there, as a stop does, and as the frame never resumes, it says so now rather than
  // as its owner destroys it.
  if (!ended && caller.stop_requested()) {
    if (slot.current == number) {
      slot.pending = false;
      slot.stopped = true;
      tell_quietly(link);
    }

    co_await async::detail::end_stopped{};
  }

  // Stopped by a later run, or ended after a later run asked it to stop: what it gave is not the last answer.
  if (!ended || slot.current != number) {
    co_return std::unexpected{std::make_exception_ptr(run_replaced{})};
  }

  // Kept as a copy, its caller given the run's own: a copy that throws, as for a lack of memory, is this run's failure,
  // which both are given, rather than thrown to its caller, and the result before is gone.
  try {
    slot.result.reset();
    slot.result.emplace(*ended);
  } catch (...) {
    auto const failure = std::current_exception();

    slot.result.emplace(std::unexpect, failure);
    ended.emplace(std::unexpect, failure);
  }

  slot.pending = false;
  tell_quietly(link);
  co_return std::move(*ended);
}

// The result of the last run of `slot`, none for no slot: std::logic_error before one ended, which `what` says, as
// asking for a value that is not there is a mistake of the caller's.
template <class R>
[[nodiscard]] std::expected<R, std::exception_ptr> const &result_of(run_slot<R> const *slot, char const *what) {
  if (slot == nullptr || !slot->result) {
    throw std::logic_error{what};
  }

  return *slot->result;
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_RUN_SLOT_HPP
