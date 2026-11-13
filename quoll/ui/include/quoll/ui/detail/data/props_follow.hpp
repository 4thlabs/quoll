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

#ifndef QUOLL_UI_DETAIL_DATA_PROPS_FOLLOW_HPP
#define QUOLL_UI_DETAIL_DATA_PROPS_FOLLOW_HPP

#include <concepts>
#include <type_traits>

namespace quoll::ui::detail {

// What an effect of a component written as a struct follows of its props: a field of them, `&props::id`, or a function
// of them, `[](props const &given) { return given.item.id; }`; its props' type, and the value followed, a copy. The
// type of a member pointer is that of the class declaring the field: one of a base names the base, which a function
// of the props reaches instead.
template <class Follow>
struct follow_traits {
  static constexpr bool valid = false;
};

template <class P, class D>
struct follow_traits<D P::*> {
  static constexpr bool valid = !std::is_function_v<D>;
  using props = P;
  using value = std::remove_cv_t<D>;
};

// A function of the props, taking them by const reference or by value.
template <class R, class P>
struct follow_function_traits {
  using props = std::remove_cvref_t<P>;
  using value = std::remove_cvref_t<R>;

  static constexpr bool valid =
      !std::is_void_v<R> && std::is_class_v<props> && (std::same_as<P, props const &> || std::same_as<P, props>);
};

template <class S>
struct follow_call_traits {
  static constexpr bool valid = false;
};

template <class C, class P, class R>
struct follow_call_traits<R (C::*)(P) const> : follow_function_traits<R, P> {};

template <class C, class P, class R>
struct follow_call_traits<R (C::*)(P) const noexcept> : follow_function_traits<R, P> {};

// A function, or a lambda whose call operator is static.
template <class P, class R>
struct follow_call_traits<R (*)(P)> : follow_function_traits<R, P> {};

template <class P, class R>
struct follow_call_traits<R (*)(P) noexcept> : follow_function_traits<R, P> {};

template <class P, class R>
struct follow_traits<R (*)(P)> : follow_call_traits<R (*)(P)> {};

template <class P, class R>
struct follow_traits<R (*)(P) noexcept> : follow_call_traits<R (*)(P) noexcept> {};

// A lambda, or a struct, with one call operator, const or static: a generic one does not say whose props it takes.
template <class Follow>
  requires std::is_class_v<Follow> && requires { &Follow::operator(); }
struct follow_traits<Follow> : follow_call_traits<decltype(&Follow::operator())> {};

template <class Follow>
concept PropsFollow = std::copy_constructible<Follow> && follow_traits<Follow>::valid;

struct follow_model {
  int id = 0;
};

struct follow_model_projection {
  int operator()(follow_model const &given) const noexcept { return given.id; }
};

static_assert(PropsFollow<int follow_model::*>);
static_assert(PropsFollow<int (*)(follow_model const &)>);
static_assert(PropsFollow<int (*)(follow_model) noexcept>);
static_assert(PropsFollow<follow_model_projection>);
static_assert(!PropsFollow<int (follow_model::*)() const>);
static_assert(!PropsFollow<void (*)(follow_model const &)>);
static_assert(!PropsFollow<int (*)(follow_model &)>);
static_assert(!PropsFollow<int>);

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_DATA_PROPS_FOLLOW_HPP
