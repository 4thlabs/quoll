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

#ifndef QUOLL_REST_RETURNS_HPP
#define QUOLL_REST_RETURNS_HPP

#include <quoll/http/status.hpp>
#include <quoll/rest/codec.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/json_codec.hpp>

#include <type_traits>

namespace quoll::rest {

/**
 * @brief An endpoint part: a declared response, with status `Status` and a `T` body encoded by `ResponseCodec` (JSON
 * by default). `T` is `void` for a response without body.
 *
 * An endpoint declares one per status. Its first one is what the service returns and what a client's `call` produces;
 * an endpoint without any answers 204 No Content.
 */
template <class T, http::status Status = http::status::ok, class ResponseCodec = json_codec>
struct returns {
  static_assert(std::is_void_v<T> || Codec<ResponseCodec, T>,
                "quoll::rest::returns: the codec cannot encode and decode the type");

  using part_category = detail::response_part;
  using type = T;
  using codec = ResponseCodec;

  static constexpr http::status status = Status;
};

} // namespace quoll::rest

#endif // QUOLL_REST_RETURNS_HPP
