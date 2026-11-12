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

#ifndef QUOLL_UI_DETAIL_DATA_RUN_SLOT_HPP
#define QUOLL_UI_DETAIL_DATA_RUN_SLOT_HPP

#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/either_stop.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/data/run_error.hpp>
#include <quoll/ui/detail/data/target.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

// The runs of an effect or an action: whether one is under way, the stop of the last one asked
// for, and how many frames of its runs are alive, ended or not: what its holder must outlive.
struct run_slot {
  bool pending = false;
  // None until a run is asked for, so that a slot made costs no allocation.
  std::stop_source last{std::nostopstate};
  std::uint32_t frames = 0;
};

// Asks for a run: the run asked for before, under way or still queued, stops at once, so that it writes nothing, and
// this one gets a stop of its own, as React's cleanup sets the ignore flag of the effect before.
[[nodiscard]] std::stop_token ask(run_slot &slot);

// A run that goes without ending, its frame destroyed from above, is under way no more and loading no more, unless a
// later run took its place. Neither copied nor moved: it lives in the run's frame.
class run_guard {
public:
  run_guard(run_slot &slot, std::stop_token mine, target_hooks target) noexcept;

  run_guard(run_guard const &) = delete;
  run_guard(run_guard &&) = delete;
  run_guard &operator=(run_guard const &) = delete;
  run_guard &operator=(run_guard &&) = delete;

  ~run_guard();

private:
  run_slot &slot_;
  std::stop_token mine_;
  target_hooks target_;
};

// Where a run's failure goes that its target does not show, as it has no place for it: to its caller in what it gives,
// or thrown, for a run whose result is dropped, an effect's or one from an event, so that the app's on_error hears of
// it. A call refused or replaced is no failure of the run, and never thrown.
enum class untold : unsigned char { given, thrown };

// Nothing to do after a success, for a run that has nothing to follow it.
struct no_follow_up {
  void operator()(auto const &...) const noexcept {}
};

// A run's failure, whatever it is and wherever it happens, an effect's start included: written into its target, which
// shows it if it has a place for it, and thrown if it has none and `failures` says so. What runs call for each failure,
// but for a lack of memory making an action's run, which throws from its caller's call. Under a lack of memory before
// a run begins, the target may show it while the run before is under way, whose answer then writes over it.
void tell_failure(target_hooks target, untold failures, std::exception_ptr const &failure);

// Runs what `make` gives for `slot`, and writes what it gives into `target`. `ask` gives the run's stop, or none to
// refuse it, run_refused at once; what it throws, as for a lack of memory, is the run's failure. A run whose stop was
// asked for, by a later one, before it starts or as it runs, gives run_replaced and writes nothing, even one whose work
// ignored the stop and came anyway. Otherwise the target says it is loading as the run starts, then holds what it
// gave, or what it threw, what `make` throws included, every failure going through tell_failure(); `follow_up` is then
// told what it gave, before its caller, and what it throws goes to the caller: given only with a target that copies
// what the run gave, an action's, as one that moves it leaves it moved from. A stop of the caller, or of its
// component, ends the caller there, as a stop does, the target loading no more. The slot and the target outlive the
// run: members of a component, which outlives its tasks. Made by its caller's call, where a lack of memory throws, as
// any call's.
template <class R, class Make, class Ask, class FollowUp = no_follow_up>
  requires std::same_as<std::invoke_result_t<Make &>, async::task<R>> &&
           std::same_as<std::invoke_result_t<Ask &>, std::optional<std::stop_token>>
async::task<std::expected<R, std::exception_ptr>> run_in(run_slot &slot, target_ref<R> target, Make make, Ask ask,
                                                         untold failures = untold::given,
                                                         FollowUp follow_up = no_follow_up{}) {
  std::optional<std::stop_token> asked;

  try {
    asked = ask();
  } catch (...) {
    auto failure = std::current_exception();

    tell_failure(target.hooks(), failures, failure);
    co_return std::unexpected{std::move(failure)};
  }

  if (!asked) {
    co_return std::unexpected{std::make_exception_ptr(run_refused{})};
  }

  auto const mine = *asked;

  // Asked for, then replaced before it started.
  if (mine.stop_requested()) {
    co_return std::unexpected{std::make_exception_ptr(run_replaced{})};
  }

  slot.pending = true;

  run_guard const guard{slot, mine, target.hooks()};

  target.hooks().begin();

  auto const caller = co_await async::current_stop_token();
  std::optional<std::expected<R, std::exception_ptr>> ended;

  // One way out for what fails once it began, the stop it runs under included, made here as it may lack memory.
  try {
    async::detail::either_stop const stops{caller, mine};
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
  // as its owner destroys it, which tasks queued meanwhile, a click or a render, would come before.
  if (!ended && caller.stop_requested()) {
    if (!mine.stop_requested()) {
      slot.pending = false;
      target.hooks().abandon();
    }

    co_await async::detail::end_stopped{};
  }

  // Stopped by a later run, or ended after a later run asked it to stop: what it gave is not the last answer.
  if (!ended || mine.stop_requested()) {
    co_return std::unexpected{std::make_exception_ptr(run_replaced{})};
  }

  slot.pending = false;

  if (*ended) {
    target.finish(*ended);
  }

  if (!*ended) {
    tell_failure(target.hooks(), failures, ended->error());
    co_return std::move(*ended);
  }

  if constexpr (std::is_void_v<R>) {
    follow_up();
  } else {
    follow_up(**ended);
  }

  co_return std::move(*ended);
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_RUN_SLOT_HPP
