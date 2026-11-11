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

#ifndef QUOLL_UI_ACTION_HPP
#define QUOLL_UI_ACTION_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/ui/detail/callback_traits.hpp>
#include <quoll/ui/detail/linked_member.hpp>
#include <quoll/ui/detail/run_slot.hpp>
#include <quoll/ui/detail/state_link.hpp>
#include <quoll/ui/run_error.hpp>

#include <cassert>
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief What a call of an action does while another is under way. */
enum class overlap {
  /** @brief It is refused: it runs nothing, its result run_refused, as a second click on a button that waits. */
  refuse,
  /** @brief It stops the one under way, whose answer then writes nothing, and runs. */
  restart,
};

namespace detail {

// What an action's arguments and result may be: kept, as values.
template <class R, class... Args>
inline constexpr bool action_signature = !std::is_reference_v<R> && !is_view<std::remove_cvref_t<R>> &&
                                         ((!std::is_reference_v<Args> && !is_view<std::remove_cvref_t<Args>>) && ...);

} // namespace detail

template <class Signature, overlap Policy = overlap::refuse>
class action;

/**
 * @brief A function a component runs, once at a time by default: whether it is under way, and the result of its last
 * run, a value or a failure, for a button to wait on and a view to show.
 *
 * @code
 * struct note_form {
 *   state<std::string> draft;
 *   action<notes::note(std::string)> add{notes_client::create};
 *
 *   element view() {
 *     auto const send = [this] {
 *       return add.run(*draft);
 *     };
 *
 *     auto const shown = add.pending() ? button("Adding…") | attribute("disabled", "")
 *                                      : button("Add") | on_click(send);
 *
 *     return div({input() | bind(draft), shown});
 *   }
 * };
 * @endcode
 *
 * A member of a component, which shows again as it changes, as a state does. run() runs the function in the task
 * awaiting it, a handler's or mounted()'s, and gives its result, a std::expected of what it gave or what it threw,
 * before giving its task too, not thrown again. A call while one is under way is refused, its result run_refused at
 * once, or, with `overlap::restart`, stops it, so that its answer writes nothing, even one that ignored the stop and
 * came anyway, and its result is run_replaced, to its caller, which goes on. A stop of the caller, as a timeout, ends
 * the run and its caller, as a stop does, and the action is under way no more, its result the one before. The result of
 * the last run that ended stays while the next is under way.
 *
 * Awaited from a task of its component, which the component outlives. Neither copied nor moved: a run under way refers
 * to it. Its arguments are copied into the run, given to the function as rvalues: none can be a reference or a view,
 * which would outlive what it refers to. Thread safety: none, as a state.
 */
template <class R, class... Args, overlap Policy>
class action<R(Args...), Policy> {
  static_assert(detail::action_signature<R, Args...>,
                "quoll::ui::action: its arguments are copied into the run, and its result kept: none can be a "
                "reference or a view");
  static_assert(std::is_void_v<R> || std::copyable<R>,
                "quoll::ui::action: its result is copied, into what it holds and what it gives");

public:
  using result_type = std::expected<R, std::exception_ptr>;
  // Given the run's own copies, as rvalues: one taking them by reference refers to the run's copy.
  using function_type = std::function<async::task<R>(Args &&...)>;

  /**
   * @brief Runs `function`, which takes the arguments and returns a task of the result.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class F>
    requires std::constructible_from<function_type, F>
  explicit action(F function) : function_(std::move(function)) {
    detail::check_function(function_, "quoll::ui::action: no function");
  }

  action(action const &) = delete;
  action(action &&) = delete;
  action &operator=(action const &) = delete;
  action &operator=(action &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~action() { assert(slot_.frames == 0 && "quoll::ui::action: destroyed while a run is under way"); }

  /**
   * @brief Runs it, as its overlap says: what it gave, or what it threw, as result() then holds; run_refused or
   * run_replaced for a call that gave no result of its own.
   */
  [[nodiscard]] async::task<result_type> run(Args... args) QUOLL_LIFETIMEBOUND {
    // Called once the run before is stopped, or not at all once refused: what it throws is this run's failure.
    auto make = [this, ... args = std::move(args)]() mutable { return function_(std::move(args)...); };

    return detail::run_in<R>(slot_, link_, std::move(make), Policy == overlap::refuse, false);
  }

  /** @brief Whether a run is under way. */
  [[nodiscard]] bool pending() const noexcept { return slot_.pending; }

  /** @brief Whether a run has ended, with a value or a failure. */
  [[nodiscard]] bool has_result() const noexcept { return slot_.result.has_value(); }

  /**
   * @brief What the last run that ended gave, or what it threw.
   * @throws std::logic_error unless has_result().
   */
  [[nodiscard]] result_type const &result() const QUOLL_LIFETIMEBOUND {
    return detail::result_of(&slot_, "quoll::ui::action: no run has ended");
  }

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

private:
  function_type function_;
  detail::run_slot<R> slot_;
  detail::state_link link_{};
};

static_assert(detail::linked_member<action<int(int)>>);
static_assert(detail::linked_member<action<void(), overlap::restart>>);

template <class Key, class Signature, overlap Policy = overlap::refuse>
class keyed_action;

/**
 * @brief An action whose runs go by key, each key once at a time by default: the saves of each product, whatever
 * product shows.
 *
 * @code
 * keyed_action<std::int64_t, products::product(products::product_update)> saves{products_client::update};
 *
 * // In a handler: co_await saves.run(id, changes); then saves.pending(id), saves.has_result(id), saves.result(id).
 * @endcode
 *
 * As an action, but for the key, which the function takes first, and its runs, kept by key: one under way for a key
 * refuses a call for that key, or is stopped by it, and leaves the others alone. Key is ordered with `<`. What a key's
 * runs left is kept as long as the action, as a detail chosen again shows it.
 *
 * Thread safety: none, as a state.
 */
template <class Key, class R, class... Args, overlap Policy>
class keyed_action<Key, R(Args...), Policy> {
  static_assert(detail::action_signature<R, Key, Args...>,
                "quoll::ui::keyed_action: its key and arguments are copied into the run, and its result kept: none "
                "can be a reference or a view");
  static_assert(std::copyable<Key> && (std::is_void_v<R> || std::copyable<R>),
                "quoll::ui::keyed_action: its key and its result are copied, into what it holds and what it gives");

public:
  using key_type = Key;
  using result_type = std::expected<R, std::exception_ptr>;
  // Given the run's own copies, as rvalues: one taking them by reference refers to the run's copy.
  using function_type = std::function<async::task<R>(Key &&, Args &&...)>;

  /**
   * @brief Runs `function`, which takes the key and the arguments and returns a task of the result.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each run.
   */
  template <class F>
    requires std::constructible_from<function_type, F>
  explicit keyed_action(F function) : function_(std::move(function)) {
    detail::check_function(function_, "quoll::ui::keyed_action: no function");
  }

  keyed_action(keyed_action const &) = delete;
  keyed_action(keyed_action &&) = delete;
  keyed_action &operator=(keyed_action const &) = delete;
  keyed_action &operator=(keyed_action &&) = delete;

  // No run's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~keyed_action() {
    for ([[maybe_unused]] auto const &[key, slot] : slots_) {
      assert(slot.frames == 0 && "quoll::ui::keyed_action: destroyed while a run is under way");
    }
  }

  /**
   * @brief Runs it for `key`, as its overlap says: what it gave, or what it threw, as result(key) then holds;
   * run_refused or run_replaced for a call that gave no result of its own.
   */
  [[nodiscard]] async::task<result_type> run(Key key, Args... args) QUOLL_LIFETIMEBOUND {
    // A map leaves its elements where they are, and none is erased: the slot outlives the run.
    auto &slot = slots_[key];

    // Called once the run before is stopped, or not at all once refused: what it throws is this run's failure.
    auto make = [this, key = std::move(key), ... args = std::move(args)]() mutable {
      return function_(std::move(key), std::move(args)...);
    };

    return detail::run_in<R>(slot, link_, std::move(make), Policy == overlap::refuse, false);
  }

  /** @brief Whether a run for `key` is under way. */
  [[nodiscard]] bool pending(Key const &key) const {
    auto const found = slots_.find(key);

    return found != slots_.end() && found->second.pending;
  }

  /** @brief Whether a run for `key` has ended, with a value or a failure. */
  [[nodiscard]] bool has_result(Key const &key) const {
    auto const found = slots_.find(key);

    return found != slots_.end() && found->second.result.has_value();
  }

  /**
   * @brief What the last run for `key` that ended gave, or what it threw.
   * @throws std::logic_error unless has_result(key).
   */
  [[nodiscard]] result_type const &result(Key const &key) const QUOLL_LIFETIMEBOUND {
    auto const found = slots_.find(key);

    return detail::result_of(found == slots_.end() ? nullptr : &found->second,
                             "quoll::ui::keyed_action: no run of that key has ended");
  }

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

private:
  function_type function_;
  std::map<Key, detail::run_slot<R>> slots_;
  detail::state_link link_{};
};

static_assert(detail::linked_member<keyed_action<int, std::string(std::string)>>);

} // namespace quoll::ui

#endif // QUOLL_UI_ACTION_HPP
