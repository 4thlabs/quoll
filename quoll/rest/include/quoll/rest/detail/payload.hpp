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

#ifndef QUOLL_REST_DETAIL_PAYLOAD_HPP
#define QUOLL_REST_DETAIL_PAYLOAD_HPP

#include <quoll/rest/bytes_codec.hpp>
#include <quoll/rest/form_codec.hpp>
#include <quoll/rest/formats.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/text_codec.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace quoll::rest::detail {

// What a value of a body or a result is, and its codec.
template <class T>
struct payload_of {
  using type = T;
  using codec = json_codec;
};

template <>
struct payload_of<text> {
  using type = std::string;
  using codec = text_codec;
};

template <>
struct payload_of<bytes> {
  using type = std::vector<std::byte>;
  using codec = bytes_codec;
};

template <class T>
struct payload_of<json<T>> {
  using type = T;
  using codec = json_codec;
};

template <class T>
struct payload_of<lenient_json<T>> {
  using type = T;
  using codec = lenient_json_codec;
};

template <class T>
struct payload_of<form<T>> {
  using type = T;
  using codec = form_codec;
};

template <class T, class C>
struct payload_of<encoded<T, C>> {
  using type = T;
  using codec = C;
};

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PAYLOAD_HPP
