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

#ifndef QUOLL_REST_FRAMING_HPP
#define QUOLL_REST_FRAMING_HPP

#include <quoll/rest/decode_error.hpp>

#include <concepts>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::rest {

/**
 * @brief How a stream of values is cut into bytes and back, for a route whose result is rest::stream<F>: a value per
 * line of JSON (json_lines), or the frames of an API of its own, as Docker's logs.
 *
 * `F::value_type` is what the stream carries, `F::content_type` the media type a server sends it with.
 * `framing.encode(value)` returns its bytes; `framing.decode(pending)` takes the first whole value off the front of
 * what has come, nothing when the rest of it has not come yet, or a decode_error for bytes that are none: what it read
 * goes from `pending`, so that a value owns what it holds, no view into it, but on an error, which a frame_stream then
 * gives with what follows as the body of its http_error. Default constructible: one is built where
 * it is used.
 */
template <class F>
concept Framing = std::default_initializable<F> &&
                  requires(F const &framing, typename F::value_type const &value, std::string &pending) {
                    { F::content_type } -> std::convertible_to<std::string_view>;
                    { framing.encode(value) } -> std::same_as<std::string>;
                    {
                      framing.decode(pending)
                    } -> std::same_as<std::expected<std::optional<typename F::value_type>, decode_error>>;
                  };

} // namespace quoll::rest

#endif // QUOLL_REST_FRAMING_HPP
