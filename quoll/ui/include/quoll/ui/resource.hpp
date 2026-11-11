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

#ifndef QUOLL_UI_RESOURCE_HPP
#define QUOLL_UI_RESOURCE_HPP

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
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::ui {

/** @brief Where a resource is. */
enum class load_status {
  /** @brief Nothing was loaded yet. */
  none,
  /** @brief A load is under way. */
  loading,
  /** @brief The last load ended, with a value or a failure: its result(). */
  done,
  /** @brief The last load was stopped by its caller, as a timeout does, before it ended: its key() only. */
  stopped,
};

/**
 * @brief What a component loads for a key: under way, done with a value or a failure, or stopped, the last load only.
 *
 * @code
 * struct dashboard_page {
 *   resource<std::int64_t, products::monthly_sales> sales{products_client::sales};
 *
 *   task<> select(std::int64_t id) {
 *     co_await sales.load(id);
 *   }
 *
 *   element view() {
 *     auto const shown = [this] {
 *       auto const &result = sales.result();
 *
 *       return result ? component<sales_chart>({.sales = *result}) : p("No sales");
 *     };
 *
 *     return div({when(sales.status() == load_status::done, shown)});
 *   }
 * };
 * @endcode
 *
 * A member of a component, which shows again as it changes, as a state does. load() runs the loader in the task
 * awaiting it, a handler's or mounted()'s, and gives its result, as result() then holds: loading again stops the load
 * before, whose answer then writes nothing, even one that ignored the stop and came anyway; that load's result is
 * run_replaced, and its caller goes on. A stop of the caller, as a
 * timeout, ends the load and its caller, as a stop does, and leaves the resource stopped; one of the component ends
 * them with it. The result of the load before goes as one starts; what the loader throws, before its task too, is the
 * failure of its result, not thrown again.
 *
 * Awaited from a task of its component, which the component outlives. Neither copied nor moved: a load under way
 * refers to it, and a component stays where it was made. Its key and value are values: a reference or a view would
 * outlive what it refers to.
 *
 * Thread safety: none, as a state: used from the app's executor.
 */
template <class Key, class T>
class resource {
  static_assert(!std::is_reference_v<Key> && !detail::is_view<std::remove_cvref_t<Key>> && !std::is_reference_v<T> &&
                    !detail::is_view<std::remove_cvref_t<T>> && !std::is_void_v<T>,
                "quoll::ui::resource: its key and its value are kept, as values");
  static_assert(std::copyable<Key> && std::copyable<T>,
                "quoll::ui::resource: its key and its value are copied, into what it holds and what it gives");

public:
  using key_type = Key;
  using value_type = T;
  using result_type = std::expected<T, std::exception_ptr>;
  // Given its own copy of the key, as an rvalue: one taking it by reference refers to the load's copy.
  using loader_type = std::function<async::task<T>(Key &&)>;

  /**
   * @brief Loads with `loader`, a function of the key returning a task of the value.
   * @throws std::invalid_argument for no function, as a null pointer, rather than at each load.
   */
  template <class F>
    requires std::constructible_from<loader_type, F>
  explicit resource(F loader) : loader_(std::move(loader)) {
    detail::check_function(loader_, "quoll::ui::resource: no loader");
  }

  resource(resource const &) = delete;
  resource(resource &&) = delete;
  resource &operator=(resource const &) = delete;
  resource &operator=(resource &&) = delete;

  // No load's frame is left: one refers to it until it goes, and its component goes once its tasks have ended.
  ~resource() { assert(slot_.frames == 0 && "quoll::ui::resource: destroyed while a load is under way"); }

  /**
   * @brief Loads for `key`, stopping the load before: what it gave, or what it threw, as result() then holds;
   * run_replaced once a later load took its place.
   */
  [[nodiscard]] async::task<result_type> load(Key key) QUOLL_LIFETIMEBOUND {
    // Called once the load before is stopped, as the load starts: what it throws, before its task, is its failure.
    auto make = [this, key = std::move(key)]() mutable {
      key_ = key;
      return loader_(std::move(key));
    };

    return detail::run_in<T>(slot_, link_, std::move(make), false, true);
  }

  /** @brief Where it is: nothing loaded, loading, done, or stopped by its caller. */
  [[nodiscard]] load_status status() const noexcept {
    if (slot_.pending) {
      return load_status::loading;
    }

    if (slot_.stopped) {
      return load_status::stopped;
    }

    return slot_.result ? load_status::done : load_status::none;
  }

  /** @brief The key of the last load, none before the first. */
  [[nodiscard]] std::optional<Key> const &key() const noexcept QUOLL_LIFETIMEBOUND { return key_; }

  /**
   * @brief What the last load gave, or what it threw.
   * @throws std::logic_error unless status() is done.
   */
  [[nodiscard]] result_type const &result() const QUOLL_LIFETIMEBOUND {
    return detail::result_of(&slot_, "quoll::ui::resource: no load is done");
  }

  /** @brief For the app of the component holding it, as a state's. */
  void link(detail::state_link to) noexcept { link_ = to; }

private:
  loader_type loader_;
  detail::run_slot<T> slot_;
  std::optional<Key> key_;
  detail::state_link link_{};
};

static_assert(detail::linked_member<resource<int, std::string>>);

} // namespace quoll::ui

#endif // QUOLL_UI_RESOURCE_HPP
