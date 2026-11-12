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

#ifndef QUOLL_UI_DETAIL_EFFECT_FUNCTION_HPP
#define QUOLL_UI_DETAIL_EFFECT_FUNCTION_HPP

#include <quoll/async/task.hpp>

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>

namespace quoll::ui::detail {

// What a state an effect depends on gives its function, and whether it gives anything.
template <class D>
struct dependency_traits {
  using value_type = D;

  [[nodiscard]] static bool holds(D const &) noexcept { return true; }

  [[nodiscard]] static D const &value(D const &dependency) noexcept { return dependency; }
};

// An optional dependency runs the effect with what it holds, and not at all while it holds none.
template <class X>
struct dependency_traits<std::optional<X>> {
  using value_type = X;

  [[nodiscard]] static bool holds(std::optional<X> const &dependency) noexcept { return dependency.has_value(); }

  [[nodiscard]] static X const &value(std::optional<X> const &dependency) noexcept { return *dependency; }
};

template <class T>
struct task_value {};

template <class V>
struct task_value<async::task<V>> {
  using type = V;
};

// What an effect may run on the values of states holding D...: a function taking them, as rvalues, and returning a
// task.
template <class F, class... D>
concept EffectFunction =
    std::copy_constructible<F> && std::invocable<F &, typename dependency_traits<D>::value_type &&...> &&
    requires { typename task_value<std::invoke_result_t<F &, typename dependency_traits<D>::value_type &&...>>::type; };

// What the task of an effect's function gives.
template <class F, class... D>
using effect_value_t =
    typename task_value<std::invoke_result_t<F &, typename dependency_traits<D>::value_type &&...>>::type;

static_assert(EffectFunction<async::task<std::string> (*)(int), std::optional<int>>);
static_assert(EffectFunction<async::task<> (*)()>);

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_EFFECT_FUNCTION_HPP
