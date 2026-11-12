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

#ifndef QUOLL_UI_DETAIL_DATA_TARGET_HPP
#define QUOLL_UI_DETAIL_DATA_TARGET_HPP

#include <quoll/ui/data/async_state.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/detail/callback_traits.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <exception>
#include <expected>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

// What an element of a target state receives, by its type: a bool the loading flag, a std::exception_ptr the error,
// anything else one of the values a run gives, in order.
enum class role : unsigned char { value, loading, error };

template <class E>
inline constexpr role role_of = std::same_as<std::remove_cv_t<E>, bool>                 ? role::loading
                                : std::same_as<std::remove_cv_t<E>, std::exception_ptr> ? role::error
                                                                                        : role::value;

// The elements of a value: a tuple's, or the value alone.
template <class T>
struct elements_of {
  using type = std::tuple<T>;
};

template <class... E>
struct elements_of<std::tuple<E...>> {
  using type = std::tuple<E...>;
};

template <class T>
inline constexpr bool is_async_value = false;

template <class V>
inline constexpr bool is_async_value<async_value<V>> = true;

// The fields of an async_value, in the order of their roles below.
template <class V, class U>
  requires std::same_as<std::remove_const_t<U>, async_value<V>>
[[nodiscard]] constexpr auto fields_of(U &value) noexcept {
  if constexpr (std::is_void_v<V>) {
    return std::tie(value.loading, value.error);
  } else {
    return std::tie(value.value, value.loading, value.error);
  }
}

// The values a run's result gives: none for void, a tuple's, or the result alone.
template <class R>
struct values_of {
  using type = std::tuple<R>;
};

template <>
struct values_of<void> {
  using type = std::tuple<>;
};

template <class... V>
struct values_of<std::tuple<V...>> {
  using type = std::tuple<V...>;
};

// Whether what a run giving R gives is kept as values: a reference or a view would refer to what the run held.
template <class R>
inline constexpr bool kept_as_values = []<class... E>(std::type_identity<std::tuple<E...>>) {
  return (!holds_view<E> && ...);
}(std::type_identity<typename values_of<R>::type>{});

// The roles of a target's elements, in order, with a lookup of each.
template <role... Roles>
struct fixed_roles {
  static constexpr std::array<role, sizeof...(Roles)> roles{Roles...};

  [[nodiscard]] static consteval std::size_t count(role wanted) {
    std::size_t found = 0;

    for (auto const each : roles) {
      found += each == wanted ? 1 : 0;
    }

    return found;
  }

  // The index of the `nth` element of that role.
  [[nodiscard]] static consteval std::size_t index(role wanted, std::size_t nth = 0) {
    for (std::size_t at = 0; at < roles.size(); ++at) {
      if (roles[at] == wanted && nth-- == 0) {
        return at;
      }
    }

    return roles.size();
  }
};

// The roles of a tuple's elements, by their type.
template <class Tuple>
struct roles_in;

template <class... E>
struct roles_in<std::tuple<E...>> : fixed_roles<role_of<E>...> {};

template <class Elements, class Roles, class Sequence>
struct value_elements;

template <class Elements, class Roles, std::size_t... Index>
struct value_elements<Elements, Roles, std::index_sequence<Index...>> {
  using type = std::tuple<std::tuple_element_t<Roles::index(role::value, Index), Elements>...>;
};

// The roles of a target state holding T: by the type of each element, or, for an async_value, by its fields, whatever
// its value is, as their names tell them apart.
template <class T>
struct roles_of {
  using elements = typename elements_of<T>::type;
  using in = roles_in<elements>;
};

template <class V>
struct roles_of<async_value<V>> {
  using elements = std::tuple<V, bool, std::exception_ptr>;
  using in = fixed_roles<role::value, role::loading, role::error>;
};

template <>
struct roles_of<async_value<>> {
  using elements = std::tuple<bool, std::exception_ptr>;
  using in = fixed_roles<role::loading, role::error>;
};

// The values a run giving R gives a target holding T: R's, or, for an async_value, R whole, a tuple too.
template <class T, class R>
struct given_values {
  using type = typename values_of<R>::type;
};

template <class V, class R>
struct given_values<async_value<V>, R> {
  using type = std::conditional_t<std::is_void_v<R>, std::tuple<>, std::tuple<R>>;
};

template <class T>
struct target_roles {
  using elements = typename roles_of<T>::elements;
  using in = typename roles_of<T>::in;

  static constexpr bool loading = in::count(role::loading) == 1;
  static constexpr bool error = in::count(role::error) == 1;
  static constexpr std::size_t values = in::count(role::value);
  static constexpr bool tuple = !std::same_as<elements, std::tuple<T>>;

  // Its values alone, in order.
  using values_type = typename value_elements<elements, in, std::make_index_sequence<values>>::type;

  // An element of `value`: of the tuple, a field of an async_value, or the value itself.
  template <std::size_t Index, class U>
    requires std::same_as<std::remove_const_t<U>, T>
  [[nodiscard]] static constexpr auto &element(U &value) noexcept {
    if constexpr (is_async_value<T>) {
      return std::get<Index>(fields_of<typename T::value_type>(value));
    } else if constexpr (tuple) {
      return std::get<Index>(value);
    } else {
      return value;
    }
  }
};

// Whether each value of a target state holding T is of the type of the one a run giving R gives, in order, copied
// when `Copied`, as a run whose caller is given them too, or moved, then moved in without throwing: what a run gave is
// written whole, or not at all. Of its type, so that no conversion makes a view of what the run held.
template <class T, class R, bool Copied>
inline constexpr bool values_writable = [] {
  using roles = target_roles<T>;
  using given = typename given_values<T, R>::type;

  if constexpr (roles::values != std::tuple_size_v<given>) {
    return false;
  } else {
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
      return (
          (std::same_as<
               std::remove_cv_t<std::tuple_element_t<Index, typename roles::values_type>>,
               std::remove_cv_t<std::tuple_element_t<Index, given>>> &&
           (!Copied || std::copy_constructible<std::tuple_element_t<Index, given>>) &&
           std::is_nothrow_move_assignable_v<std::tuple_element_t<Index, typename roles::values_type>>) &&
          ...
      );
    }(std::make_index_sequence<roles::values>{});
  }
}();

// A target state holding T, for a run giving R, said one rule at a time: one loading flag and one error at most, and
// as many values as R gives, each of the type of one of R's, in order, none a bool or an error, which a target would
// not tell apart from its flags. A run whose caller is given what it gave, `Copied`, as an action's, may also write its
// flags alone.
template <class T, class R, bool Copied>
consteval void check_target() {
  using in = typename target_roles<T>::in;
  using given = typename given_values<T, R>::type;

  static_assert(in::count(role::loading) <= 1, "quoll::ui: a target state holds one bool at most, its loading flag");
  static_assert(
      in::count(role::error) <= 1, "quoll::ui: a target state holds one std::exception_ptr at most, its error"
  );
  static_assert(
      std::is_void_v<R> || std::is_nothrow_move_constructible_v<R>,
      "quoll::ui: what a run gives moves without throwing, once its target holds it, so that the run ends "
      "on what its target holds"
  );

  if constexpr (!Copied || target_roles<T>::values > 0) {
    static_assert(
        is_async_value<T> || roles_in<given>::count(role::value) == std::tuple_size_v<given>,
        "quoll::ui: a function's values cannot be a bool or a std::exception_ptr, which a target state "
        "reads as its loading flag or its error"
    );
    static_assert(
        target_roles<T>::values == std::tuple_size_v<given>,
        "quoll::ui: a target state holds as many values as its function gives, in order"
    );
    static_assert(
        values_writable<T, R, Copied>,
        "quoll::ui: each value of a target state is of the type its function gives, in order, and moved "
        "in without throwing"
    );
  }
}

// What runs do to a target state, the same whatever they give.
struct target_table {
  // Started: loading, and no error.
  void (*begin)(void *target) noexcept;
  // Ended with nothing to write, as a stop of its caller: loading no more.
  void (*abandon)(void *target) noexcept;
  // Failed: loading no more, and the error, where it has a place for it.
  void (*fail)(void *target, std::exception_ptr failure) noexcept;
};

// What a run giving R does to a target state as it ends well.
template <class R>
struct typed_target_table {
  target_table common;
  // Its values written, all at once, loading no more. A value that throws as it is made, as a copy lacking memory, is
  // the run's failure, which `ended` then holds, the target left as it was, for its runner to write as any failure.
  void (*finish)(void *target, std::expected<R, std::exception_ptr> &ended) noexcept;
};

// Writes into `target` and tells its app, a render that cannot be asked for now asked by the next change: the write
// itself, `change`, throws nothing.
template <class T, class Change>
void write_quietly(state<T> &target, Change change) noexcept {
  try {
    target.update(std::move(change));
  } catch (...) { // NOLINT(bugprone-empty-catch): the next change asks for the render again.
  }
}

// The flags of `value`, as a run leaves them: loading or not, and no error.
template <class T>
void set_flags(T &value, bool loading) noexcept {
  using roles = target_roles<T>;

  if constexpr (roles::loading) {
    roles::template element<roles::in::index(role::loading)>(value) = loading;
  }

  if constexpr (roles::error) {
    roles::template element<roles::in::index(role::error)>(value) = nullptr;
  }
}

template <class T>
void begin_target(void *target) noexcept {
  using roles = target_roles<T>;

  if constexpr (roles::loading || roles::error) {
    auto &held = *static_cast<state<T> *>(target);
    auto const &now = *held;
    bool unchanged = true;

    if constexpr (roles::loading) {
      unchanged = unchanged && roles::template element<roles::in::index(role::loading)>(now);
    }

    if constexpr (roles::error) {
      unchanged = unchanged && roles::template element<roles::in::index(role::error)>(now) == nullptr;
    }

    if (!unchanged) {
      write_quietly(held, [](T &value) noexcept { set_flags(value, true); });
    }
  }
}

template <class T>
void abandon_target(void *target) noexcept {
  using roles = target_roles<T>;

  if constexpr (roles::loading) {
    auto &held = *static_cast<state<T> *>(target);

    if (roles::template element<roles::in::index(role::loading)>(*held)) {
      write_quietly(held, [](T &value) noexcept {
        roles::template element<roles::in::index(role::loading)>(value) = false;
      });
    }
  }
}

template <class T>
void fail_target(void *target, std::exception_ptr failure) noexcept {
  using roles = target_roles<T>;

  if constexpr (roles::loading || roles::error) {
    write_quietly(*static_cast<state<T> *>(target), [&failure](T &value) noexcept {
      if constexpr (roles::loading) {
        roles::template element<roles::in::index(role::loading)>(value) = false;
      }

      if constexpr (roles::error) {
        roles::template element<roles::in::index(role::error)>(value) = std::move(failure);
      }
    });
  }
}

// The value of `given` at `Index`: of a tuple, or the value itself.
template <class T, std::size_t Index, class R>
[[nodiscard]] auto &value_at(std::expected<R, std::exception_ptr> &given) noexcept {
  if constexpr (std::same_as<typename given_values<T, R>::type, std::tuple<R>>) {
    return *given;
  } else {
    return std::get<Index>(*given);
  }
}

// The values of a target holding T, made from what a run gave: copied, or moved.
template <class T, class R, bool Copied, std::size_t... Index>
[[nodiscard]] typename target_roles<T>::values_type
made_values(std::expected<R, std::exception_ptr> &given, std::index_sequence<Index...>) {
  using values = typename target_roles<T>::values_type;

  if constexpr (Copied) {
    return values(std::as_const(value_at<T, Index>(given))...);
  } else {
    return values(std::move(value_at<T, Index>(given))...);
  }
}

template <class T, class R, bool Copied>
void finish_target(void *target, std::expected<R, std::exception_ptr> &ended) noexcept {
  using roles = target_roles<T>;
  using sequence = std::make_index_sequence<roles::values>;

  if constexpr (roles::values == 0 && !roles::loading && !roles::error) {
    return;
  } else {
    std::optional<typename roles::values_type> made;

    try {
      made.emplace(made_values<T, R, Copied>(ended, sequence{}));
    } catch (...) {
      auto const failure = std::current_exception();

      ended = std::unexpected{failure};
      return;
    }

    write_quietly(*static_cast<state<T> *>(target), [&made](T &value) noexcept {
      [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((roles::template element<roles::in::index(role::value, Index)>(value) = std::move(std::get<Index>(*made))),
         ...);
      }(sequence{});

      set_flags(value, false);
    });
  }
}

template <class T>
inline constexpr target_table target_table_for{
    .begin = &begin_target<T>,
    .abandon = &abandon_target<T>,
    .fail = &fail_target<T>,
};

template <class T, class R, bool Copied>
inline constexpr typed_target_table<R> typed_target_table_for{
    .common = target_table_for<T>,
    .finish = &finish_target<T, R, Copied>,
};

// A target state a run writes into, or none: erased, so that an effect and an action need not name its type.
class target_hooks {
public:
  target_hooks() = default;

  target_hooks(void *target, target_table const *table) noexcept : target_(target), table_(table) {}

  void begin() const noexcept {
    if (table_ != nullptr) {
      table_->begin(target_);
    }
  }

  void abandon() const noexcept {
    if (table_ != nullptr) {
      table_->abandon(target_);
    }
  }

  void fail(std::exception_ptr failure) const noexcept {
    if (table_ != nullptr) {
      table_->fail(target_, std::move(failure));
    }
  }

private:
  void *target_ = nullptr;
  target_table const *table_ = nullptr;
};

// The target of runs giving R, which copy what they gave when `Copied`, as an action's, whose caller is given it too.
template <class R>
class target_ref {
public:
  target_ref() = default;

  template <class T, bool Copied>
  target_ref(state<T> &target, std::bool_constant<Copied>) noexcept
      : target_(&target), table_(&typed_target_table_for<T, R, Copied>) {}

  [[nodiscard]] target_hooks hooks() const noexcept {
    return table_ == nullptr ? target_hooks{} : target_hooks{target_, &table_->common};
  }

  void finish(std::expected<R, std::exception_ptr> &ended) const noexcept {
    if (table_ != nullptr) {
      table_->finish(target_, ended);
    }
  }

private:
  void *target_ = nullptr;
  typed_target_table<R> const *table_ = nullptr;
};

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_TARGET_HPP
