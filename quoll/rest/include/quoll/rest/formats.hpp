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

#ifndef QUOLL_REST_FORMATS_HPP
#define QUOLL_REST_FORMATS_HPP

#include <quoll/rest/codec.hpp>

namespace quoll::rest {

/** @brief A body or a result of plain text, `text/plain`, as a std::string. */
struct text {};

/** @brief A body or a result of bytes, `application/octet-stream`, as a std::vector<std::byte>. */
struct bytes {};

/** @brief A body or a result in JSON, what a type alone already is: to say it. */
template <class T>
struct json {};

/** @brief A result in JSON read leniently: the fields its type does not know are skipped, as an API adds some. */
template <class T>
struct lenient_json {};

/** @brief A body of a form, `application/x-www-form-urlencoded`: the fields of `T` by name (see form_codec). */
template <class T>
struct form {};

/**
 * @brief A body or a result encoded by a codec of one's own, `rest::encoded<T, protobuf_codec>`: `json<T>`, `text`,
 * `bytes` and `form<T>` are its common cases.
 */
template <class T, class C>
struct encoded {
  static_assert(Codec<C, T>, "quoll::rest::encoded: the codec cannot encode and decode the type");
};

} // namespace quoll::rest

#endif // QUOLL_REST_FORMATS_HPP
