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

#ifndef QUOLL_HTTP_DETAIL_HAS_SIGNATURE_HPP
#define QUOLL_HTTP_DETAIL_HAS_SIGNATURE_HPP

#include <tuple>

namespace quoll::http::detail {

template <class Result, class... Arguments>
struct signature {
  using result = Result;
  using arguments = std::tuple<Arguments...>;
};

// The signature of a handler: a function, or an object with a single operator(), which `auto` parameters do not give.
template <class T>
struct method_signature {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...)> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) const> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) noexcept> : signature<R, A...> {};

template <class C, class R, class... A>
struct method_signature<R (C::*)(A...) const noexcept> : signature<R, A...> {};

// A static operator(), as a `static` lambda has.
template <class R, class... A>
struct method_signature<R (*)(A...)> : signature<R, A...> {};

template <class R, class... A>
struct method_signature<R (*)(A...) noexcept> : signature<R, A...> {};

template <class F>
struct signature_of {};

template <class F>
  requires requires { &F::operator(); }
struct signature_of<F> : method_signature<decltype(&F::operator())> {};

template <class R, class... A>
struct signature_of<R (*)(A...)> : signature<R, A...> {};

template <class R, class... A>
struct signature_of<R (*)(A...) noexcept> : signature<R, A...> {};

// What a handler's parameters can be read from: not a generic lambda, which has no one signature.
template <class F>
concept HasSignature = requires { typename signature_of<F>::arguments; };

static_assert(HasSignature<int (*)(char)>);
static_assert(!HasSignature<int>);

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_HAS_SIGNATURE_HPP
