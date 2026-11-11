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
#include <quoll/ui/detail/target.hpp>
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

// A function to run, refused empty rather than at each run.
template <class Function>
void check_function(Function const &function, char const *message) {
  if (!function) {
    throw std::invalid_argument{message};
  }
}

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

// Runs what `make` gives for `slot`, and writes what it gives into `target`. `ask` gives the run's stop, or none to
// refuse it, run_refused at once; what it throws, as for a lack of memory, is the run's failure. A run whose stop was
// asked for, by a later one, before it starts or as it runs, gives run_replaced and writes nothing, even one whose work
// ignored the stop and came anyway. Otherwise the target says it is loading as the run starts, then holds what it
// gave, or what it threw, what `make` throws included. A stop of the caller, or of its component, ends the caller
// there, as a stop does, the target loading no more. The slot and the target outlive the run: members of a component,
// which outlives its tasks.
template <class R, class Make, class Ask>
  requires std::same_as<std::invoke_result_t<Make &>, async::task<R>> &&
           std::same_as<std::invoke_result_t<Ask &>, std::optional<std::stop_token>>
async::task<std::expected<R, std::exception_ptr>> run_in(run_slot &slot, target_ref<R> target, Make make, Ask ask) {
  std::optional<std::stop_token> asked;

  try {
    asked = ask();
  } catch (...) {
    co_return std::unexpected{std::current_exception()};
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
  } else {
    static_cast<void>(target.hooks().fail(ended->error()));
  }

  co_return std::move(*ended);
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_RUN_SLOT_HPP
