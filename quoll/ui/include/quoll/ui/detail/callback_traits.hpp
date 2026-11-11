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

#ifndef QUOLL_UI_DETAIL_CALLBACK_TRAITS_HPP
#define QUOLL_UI_DETAIL_CALLBACK_TRAITS_HPP

#include <functional>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <type_traits>

namespace quoll::ui {

template <class Signature>
class callback;

} // namespace quoll::ui

namespace quoll::ui::detail {

template <class T>
inline constexpr bool is_callback = false;

template <class Signature>
inline constexpr bool is_callback<callback<Signature>> = true;

// A view of std::ranges that owns what it gives, or gives what it computes, which a copy makes the task's own.
template <class T>
inline constexpr bool owns_its_elements = false;

template <class T>
inline constexpr bool owns_its_elements<std::optional<T>> = true;

template <class T>
inline constexpr bool owns_its_elements<std::ranges::single_view<T>> = true;

template <class R>
inline constexpr bool owns_its_elements<std::ranges::owning_view<R>> = true;

template <class T>
inline constexpr bool owns_its_elements<std::ranges::empty_view<T>> = true;

template <class W, class Bound>
inline constexpr bool owns_its_elements<std::ranges::iota_view<W, Bound>> = true;

// What may refer to an object it does not hold, which a copy does not make the task's own: a view of std::ranges, as
// std::string_view, std::span, or a transform_view over a container, but for those owning their elements; a
// std::reference_wrapper; a std::initializer_list.
template <class T>
inline constexpr bool is_view = std::ranges::view<T> && !owns_its_elements<T>;

template <class T>
inline constexpr bool is_view<std::reference_wrapper<T>> = true;

template <class T>
inline constexpr bool is_view<std::initializer_list<T>> = true;

// What may hold no function, which a callback made of it holds none either.
template <class T>
inline constexpr bool may_be_empty = std::is_pointer_v<T> || std::is_member_pointer_v<T>;

template <class Signature>
inline constexpr bool may_be_empty<std::function<Signature>> = true;

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_CALLBACK_TRAITS_HPP
