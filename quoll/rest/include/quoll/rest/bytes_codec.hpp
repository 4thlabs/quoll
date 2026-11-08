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

#ifndef QUOLL_REST_BYTES_CODEC_HPP
#define QUOLL_REST_BYTES_CODEC_HPP

#include <quoll/rest/codec.hpp>
#include <quoll/rest/decode_error.hpp>

#include <concepts>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::rest {

namespace detail {

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view body);

} // namespace detail

/**
 * @brief Bodies as they are, `application/octet-stream`: a `std::vector<std::byte>`, or a `std::string` holding
 * bytes rather than text. A body of another media type (an image sent as `image/png`) takes a codec of its own, with
 * its content type: a codec has one.
 */
class bytes_codec {
public:
  static constexpr std::string_view content_type = "application/octet-stream";

  [[nodiscard]] std::string encode(std::vector<std::byte> const &value) const;

  [[nodiscard]] std::string encode(std::string const &value) const;

  /** @brief The bytes of `body`: any body decodes. */
  template <class T>
    requires std::same_as<T, std::vector<std::byte>> || std::same_as<T, std::string>
  [[nodiscard]] std::expected<T, decode_error> decode(std::string_view body) const {
    if constexpr (std::same_as<T, std::string>) {
      return std::string{body};
    } else {
      return detail::bytes_of(body);
    }
  }
};

static_assert(Codec<bytes_codec, std::vector<std::byte>>);
static_assert(Codec<bytes_codec, std::string>);

} // namespace quoll::rest

#endif // QUOLL_REST_BYTES_CODEC_HPP
