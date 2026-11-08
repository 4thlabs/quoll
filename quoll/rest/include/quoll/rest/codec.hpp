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

#ifndef QUOLL_REST_CODEC_HPP
#define QUOLL_REST_CODEC_HPP

#include <quoll/rest/decode_error.hpp>

#include <concepts>
#include <expected>
#include <string>
#include <string_view>

namespace quoll::rest {

/**
 * @brief Encodes values of type `T` into bodies and decodes bodies back, for one media type.
 *
 * `codec.content_type` is the media type sent in `Content-Type`. `codec.encode(value)` returns the body; a value that
 * cannot be encoded throws. `codec.template decode<T>(body)` returns the value, or a decode_error for a body that does
 * not match the type: bodies come from the network. A codec is default constructible: one is built where it is used.
 */
template <class C, class T>
concept Codec = std::default_initializable<C> && requires(C const &codec, T const &value, std::string_view body) {
  { codec.content_type } -> std::convertible_to<std::string_view>;
  { codec.encode(value) } -> std::same_as<std::string>;
  { codec.template decode<T>(body) } -> std::same_as<std::expected<T, decode_error>>;
};

} // namespace quoll::rest

#endif // QUOLL_REST_CODEC_HPP
